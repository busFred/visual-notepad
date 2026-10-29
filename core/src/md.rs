//! Helpers shared by the comrak based parsers: options, source positions and
//! plain-text extraction.

use comrak::Options;
use comrak::nodes::{AstNode, LineColumn, NodeValue};

/// GFM plus math and front matter, matching what the VS Code Markdown editor
/// accepted. Typographer is applied while rendering, not by the parser, so
/// that heading anchors stay independent of that setting.
pub fn options(math: bool, linkify: bool) -> Options<'static> {
    let mut options = Options::default();
    options.extension.table = true;
    options.extension.strikethrough = true;
    options.extension.autolink = linkify;
    options.extension.tasklist = true;
    options.extension.footnotes = true;
    options.extension.math_dollars = math;
    options.extension.front_matter_delimiter = Some("---".to_string());
    options
}

/// Byte offset of the first character of every line (lines end with `\n`,
/// `\r\n` or a lone `\r`).
pub fn line_starts(src: &str) -> Vec<usize> {
    let bytes = src.as_bytes();
    let mut starts = vec![0];
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\n' => starts.push(i + 1),
            b'\r' => {
                if bytes.get(i + 1) == Some(&b'\n') {
                    i += 1;
                }
                starts.push(i + 1);
            }
            _ => {}
        }
        i += 1;
    }
    starts
}

/// Byte offset of a 1-based comrak line and byte column.
pub fn byte_offset(starts: &[usize], pos: LineColumn) -> Option<usize> {
    if pos.line == 0 || pos.column == 0 {
        return None;
    }
    starts.get(pos.line - 1).map(|s| s + pos.column - 1)
}

/// 0-based first and last line (inclusive) of a node.
pub fn line_range<'a>(node: &'a AstNode<'a>) -> (usize, usize) {
    let sp = node.data().sourcepos;
    let first = sp.start.line.saturating_sub(1);
    // Some blocks end at column 0 of the line after their content.
    let last = if sp.end.column == 0 {
        sp.end.line.saturating_sub(2)
    } else {
        sp.end.line.saturating_sub(1)
    };
    (first, last.max(first))
}

/// Plain text of inline content, markup stripped.
pub fn plain_text<'a>(node: &'a AstNode<'a>, out: &mut String) {
    for child in node.children() {
        match &child.data().value {
            NodeValue::Text(t) => out.push_str(t),
            NodeValue::Code(c) => out.push_str(&c.literal),
            NodeValue::Math(m) => out.push_str(&m.literal),
            NodeValue::SoftBreak | NodeValue::LineBreak => out.push('\n'),
            NodeValue::HtmlInline(_) | NodeValue::FootnoteReference(_) => {}
            _ => plain_text(child, out),
        }
    }
}
