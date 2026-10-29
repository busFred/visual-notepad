//! Splits a Markdown document into top-level blocks with their source lines
//! and renders each block to HTML that Qt's rich text engine understands.
//! Code blocks, display math and front matter are returned as raw text so
//! the C++ side can draw them (syntax highlighting, math, diagrams).

use std::collections::HashMap;

use comrak::nodes::{AstNode, NodeValue, TableAlignment};
use comrak::{Arena, parse_document};

use crate::ffi::{Block, BlockKind, RenderOptions};
use crate::{links, md};

pub fn blocks(markdown: &str, options: &RenderOptions) -> Vec<Block> {
    let arena = Arena::new();
    let root = parse_document(
        &arena,
        markdown,
        &md::options(options.math, options.linkify),
    );
    let starts = md::line_starts(markdown);
    let mut covered = vec![false; starts.len()];

    // comrak moves footnote definitions to the end of the document; the
    // numbering follows their order in the source.
    let mut footnotes: Vec<(usize, String)> = Vec::new();
    for node in root.children() {
        if let NodeValue::FootnoteDefinition(f) = &node.data().value {
            footnotes.push((node.data().sourcepos.start.line, f.name.to_lowercase()));
        }
    }
    footnotes.sort();
    let footnotes: Vec<String> = footnotes.into_iter().map(|(_, name)| name).collect();

    let mut renderer = Html {
        options,
        footnotes: &footnotes,
        out: String::new(),
        inline_math: Vec::new(),
        slugs: HashMap::new(),
        base_line: 0,
    };

    let mut result = Vec::new();
    for node in root.children() {
        let (first_line, last_line) = md::line_range(node);
        for flag in covered.iter_mut().take(last_line + 1).skip(first_line) {
            *flag = true;
        }
        let mut block = Block {
            kind: BlockKind::Html,
            first_line,
            last_line,
            html: String::new(),
            lang: String::new(),
            text: String::new(),
            inline_math: Vec::new(),
        };
        let data = node.data();
        match &data.value {
            NodeValue::CodeBlock(code) => {
                block.kind = BlockKind::Code;
                block.lang = code
                    .info
                    .split_whitespace()
                    .next()
                    .unwrap_or_default()
                    .to_string();
                block.text = strip_newline(&code.literal).to_string();
            }
            NodeValue::FrontMatter(raw) => {
                front_matter(&mut block, &front_matter_body(raw), options)
            }
            _ => {
                if let Some(tex) = display_math(node) {
                    block.kind = BlockKind::Math;
                    block.text = tex;
                } else {
                    renderer.out.clear();
                    renderer.inline_math.clear();
                    renderer.base_line = first_line;
                    renderer.node(node);
                    block.html = std::mem::take(&mut renderer.out);
                    block.inline_math = std::mem::take(&mut renderer.inline_math);
                }
            }
        }
        result.push(block);
    }

    // Definitions are not in the tree; they are the leftover lines.
    for def in links::definitions(markdown, &starts, &covered) {
        result.push(Block {
            kind: BlockKind::Hidden,
            first_line: def.first_line,
            last_line: def.last_line,
            html: String::new(),
            lang: String::new(),
            text: String::new(),
            inline_math: Vec::new(),
        });
    }
    result.sort_by_key(|b| b.first_line);
    result
}

fn strip_newline(text: &str) -> &str {
    text.strip_suffix('\n')
        .map_or(text, |t| t.strip_suffix('\r').unwrap_or(t))
}

/// The YAML between the `---` lines of a front matter node.
fn front_matter_body(raw: &str) -> String {
    let mut lines: Vec<&str> = raw.lines().collect();
    if !lines.is_empty() {
        lines.remove(0);
    }
    while lines.last().is_some_and(|l| l.trim().is_empty()) {
        lines.pop();
    }
    if lines.last().is_some_and(|l| l.trim_end() == "---") {
        lines.pop();
    }
    lines.join("\n")
}

/// TeX of a paragraph that consists of one multi-line `$$` display math.
fn display_math<'a>(node: &'a AstNode<'a>) -> Option<String> {
    if !matches!(node.data().value, NodeValue::Paragraph) {
        return None;
    }
    let sp = node.data().sourcepos;
    let child = node.first_child()?;
    if sp.start.line == sp.end.line || child.next_sibling().is_some() {
        return None;
    }
    match &child.data().value {
        NodeValue::Math(m) if m.display_math => Some(m.literal.trim_matches('\n').to_string()),
        _ => None,
    }
}

fn front_matter(block: &mut Block, value: &str, options: &RenderOptions) {
    block.text = value.to_string();
    match options.front_matter {
        0 => block.kind = BlockKind::Hidden,
        2 => {
            // Simple "key: value" lines become a table; anything nested is
            // shown as code instead.
            let mut rows = String::new();
            // The body starts on the line after the opening `---`.
            for (offset, line) in value
                .lines()
                .enumerate()
                .map(|(i, l)| (i + 1, l))
                .filter(|(_, l)| !l.trim().is_empty())
            {
                let Some((key, val)) = line.split_once(':') else {
                    block.kind = BlockKind::FrontMatter;
                    return;
                };
                if line.starts_with([' ', '\t', '-']) {
                    block.kind = BlockKind::FrontMatter;
                    return;
                }
                rows.push_str(&format!(
                    "<tr><th>{}{}</th><td>{}</td></tr>",
                    line_marker(offset),
                    escape(key.trim()),
                    escape(val.trim().trim_matches('"'))
                ));
            }
            block.kind = BlockKind::Html;
            block.html = format!(
                "<table class=\"frontmatter\" cellspacing=\"0\" cellpadding=\"4\">{rows}</table>"
            );
        }
        _ => block.kind = BlockKind::FrontMatter,
    }
}

/// An empty named anchor recording that the rendered text after it starts
/// source line `offset` (relative to the block's first line). The C++ side
/// reads these positions back from the laid-out document to keep the source
/// and preview scrolled to the same line. Empty anchors take no space, add no
/// text and have no href, so they are invisible and are not links. The colon
/// keeps them apart from heading slugs.
fn line_marker(offset: usize) -> String {
    format!("<a name=\"vn-line:{offset}\"></a>")
}

pub fn escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(c),
        }
    }
    out
}

/// GitHub-style heading anchor.
pub fn slug(text: &str) -> String {
    text.trim()
        .to_lowercase()
        .chars()
        .filter_map(|c| {
            if c.is_alphanumeric() || c == '-' || c == '_' {
                Some(c)
            } else if c.is_whitespace() {
                Some('-')
            } else {
                None
            }
        })
        .collect()
}

/// Smart quotes and dashes, like markdown-it's typographer.
fn typographer(text: &str) -> String {
    let text = text
        .replace("---", "\u{2014}")
        .replace("--", "\u{2013}")
        .replace("...", "\u{2026}");
    let mut out = String::with_capacity(text.len());
    let mut previous: Option<char> = None;
    for c in text.chars() {
        let opening = previous.is_none_or(|p| p.is_whitespace() || "([{".contains(p));
        match c {
            '"' => out.push(if opening { '\u{201C}' } else { '\u{201D}' }),
            '\'' => out.push(if opening { '\u{2018}' } else { '\u{2019}' }),
            _ => out.push(c),
        }
        previous = Some(c);
    }
    out
}

struct Html<'a> {
    options: &'a RenderOptions,
    footnotes: &'a [String],
    out: String,
    inline_math: Vec<String>,
    slugs: HashMap<String, usize>,
    /// First source line of the block being rendered.
    base_line: usize,
}

impl Html<'_> {
    /// Children, with runs of text merged so that the typographer sees whole
    /// sentences.
    fn children<'a>(&mut self, node: &'a AstNode<'a>) {
        let mut text = String::new();
        for child in node.children() {
            match &child.data().value {
                NodeValue::Text(t) => text.push_str(t),
                NodeValue::SoftBreak => text.push('\n'),
                _ => {
                    self.text(&std::mem::take(&mut text));
                    self.node(child);
                }
            }
        }
        self.text(&text);
    }

    fn text(&mut self, value: &str) {
        if value.is_empty() {
            return;
        }
        let escaped = if self.options.typographer {
            escape(&typographer(value))
        } else {
            escape(value)
        };
        if self.options.breaks {
            self.out.push_str(&escaped.replace('\n', "<br />"));
        } else {
            self.out.push_str(&escaped);
        }
    }

    fn link_open(&mut self, url: &str, title: &str) {
        self.out.push_str(&format!("<a href=\"{}\"", escape(url)));
        if !title.is_empty() {
            self.out.push_str(&format!(" title=\"{}\"", escape(title)));
        }
        self.out.push('>');
    }

    fn wrap<'a>(&mut self, node: &'a AstNode<'a>, tag: &str) {
        self.out.push_str(&format!("<{tag}>"));
        self.children(node);
        self.out.push_str(&format!("</{tag}>"));
    }

    /// Marks where the node's first source line starts in the output; the
    /// block's own first line needs no marker.
    fn marker<'a>(&mut self, node: &'a AstNode<'a>) {
        self.marker_at(node.data().sourcepos.start.line.saturating_sub(1));
    }

    fn marker_at(&mut self, line: usize) {
        if line > self.base_line {
            self.out.push_str(&line_marker(line - self.base_line));
        }
    }

    fn footnote_number(&self, id: &str) -> usize {
        self.footnotes
            .iter()
            .position(|x| x == id)
            .map_or(0, |i| i + 1)
    }

    fn node<'a>(&mut self, node: &'a AstNode<'a>) {
        let data = node.data();
        match &data.value {
            NodeValue::Paragraph => {
                if let Some(tex) = display_math(node) {
                    // Display math nested in a list or quote.
                    self.out.push_str("<p align=\"center\">");
                    self.marker(node);
                    self.out.push_str(&format!(
                        "<img src=\"vn-math-display:{}\" /></p>",
                        self.inline_math.len()
                    ));
                    self.inline_math.push(tex);
                } else {
                    self.out.push_str("<p>");
                    self.marker(node);
                    self.children(node);
                    self.out.push_str("</p>");
                }
            }
            NodeValue::Heading(h) => {
                let mut plain = String::new();
                md::plain_text(node, &mut plain);
                let mut slug = slug(&plain);
                let count = self.slugs.entry(slug.clone()).or_insert(0);
                if *count > 0 {
                    slug = format!("{slug}-{count}");
                }
                *count += 1;
                self.out.push_str(&format!(
                    "<h{d}><a name=\"{}\"></a>",
                    escape(&slug),
                    d = h.level
                ));
                self.marker(node);
                self.children(node);
                self.out.push_str(&format!("</h{}>", h.level));
            }
            NodeValue::Text(t) => self.text(t),
            NodeValue::SoftBreak => self.text("\n"),
            NodeValue::LineBreak => self.out.push_str("<br />"),
            NodeValue::Emph => self.wrap(node, "em"),
            NodeValue::Strong => self.wrap(node, "strong"),
            NodeValue::Strikethrough => self.wrap(node, "s"),
            NodeValue::Code(c) => self
                .out
                .push_str(&format!("<code>{}</code>", escape(&c.literal))),
            NodeValue::Math(m) => {
                self.out.push_str(&format!(
                    "<img src=\"vn-math:{}\" style=\"vertical-align: middle\" />",
                    self.inline_math.len()
                ));
                self.inline_math
                    .push(m.literal.trim_matches('\n').to_string());
            }
            NodeValue::Link(l) => {
                self.link_open(&l.url, &l.title);
                self.children(node);
                self.out.push_str("</a>");
            }
            NodeValue::Image(i) => {
                let mut alt = String::new();
                md::plain_text(node, &mut alt);
                self.out.push_str(&format!(
                    "<img src=\"{}\" alt=\"{}\"",
                    escape(&i.url),
                    escape(&alt)
                ));
                if !i.title.is_empty() {
                    self.out
                        .push_str(&format!(" title=\"{}\"", escape(&i.title)));
                }
                self.out.push_str(" />");
            }
            NodeValue::FootnoteReference(f) => {
                let id = f.name.to_lowercase();
                let n = self.footnote_number(&id);
                self.out.push_str(&format!(
                    "<sup><a href=\"#fn-{}\">{}</a></sup>",
                    escape(&slug(&id)),
                    if n > 0 { n.to_string() } else { escape(&id) }
                ));
            }
            NodeValue::FootnoteDefinition(f) => {
                let id = f.name.to_lowercase();
                self.out.push_str(&format!(
                    "<div class=\"footnote\"><a name=\"fn-{}\"></a><sup>{}</sup> ",
                    escape(&slug(&id)),
                    self.footnote_number(&id)
                ));
                // The first paragraph's text follows the number on the same
                // line; Qt would start a new block for a <p>.
                for (i, child) in node.children().enumerate() {
                    if i == 0 && matches!(child.data().value, NodeValue::Paragraph) {
                        self.children(child);
                    } else {
                        self.node(child);
                    }
                }
                self.out.push_str("</div>");
            }
            NodeValue::BlockQuote => self.wrap(node, "blockquote"),
            NodeValue::List(l) => {
                let tasks = node
                    .children()
                    .all(|i| matches!(i.data().value, NodeValue::TaskItem(_)));
                let style = if tasks {
                    " style=\"list-style-type: none\""
                } else {
                    ""
                };
                let ordered = l.list_type == comrak::nodes::ListType::Ordered;
                if ordered {
                    self.out
                        .push_str(&format!("<ol start=\"{}\"{style}>", l.start));
                } else {
                    self.out.push_str(&format!("<ul{style}>"));
                }
                for item in node.children() {
                    self.item(item, l.tight);
                }
                self.out.push_str(if ordered { "</ol>" } else { "</ul>" });
            }
            NodeValue::Item(_) | NodeValue::TaskItem(_) => self.item(node, false),
            NodeValue::CodeBlock(c) => {
                self.out.push_str("<pre>");
                self.marker(node);
                self.out.push_str(&escape(c.literal.trim_end_matches('\n')));
                self.out.push_str("</pre>");
            }
            NodeValue::HtmlBlock(h) => self.out.push_str(h.literal.trim_end_matches(['\n', '\r'])),
            NodeValue::HtmlInline(h) => self.out.push_str(h),
            NodeValue::ThematicBreak => self.out.push_str("<hr />"),
            NodeValue::Table(t) => {
                self.out
                    .push_str("<table cellspacing=\"0\" cellpadding=\"5\">");
                for (r, row) in node.children().enumerate() {
                    self.out.push_str("<tr>");
                    for (c, cell) in row.children().enumerate() {
                        let tag = if r == 0 { "th" } else { "td" };
                        let align = match t.alignments.get(c) {
                            Some(TableAlignment::Left) => " align=\"left\"",
                            Some(TableAlignment::Right) => " align=\"right\"",
                            Some(TableAlignment::Center) => " align=\"center\"",
                            _ => "",
                        };
                        self.out.push_str(&format!("<{tag}{align}>"));
                        // The header row also stands for the delimiter line
                        // below it, which is not rendered.
                        if c == 0 {
                            self.marker(row);
                        }
                        self.children(cell);
                        self.out.push_str(&format!("</{tag}>"));
                    }
                    self.out.push_str("</tr>");
                }
                self.out.push_str("</table>");
            }
            NodeValue::FrontMatter(_) => {}
            _ => self.children(node),
        }
    }

    fn item<'a>(&mut self, node: &'a AstNode<'a>, tight: bool) {
        self.out.push_str("<li>");
        self.marker(node);
        if let NodeValue::TaskItem(task) = &node.data().value {
            let line = node.data().sourcepos.start.line.saturating_sub(1);
            self.out.push_str(&format!(
                "<a class=\"task\" href=\"vn-task:{line}\">{}</a> ",
                if task.symbol.is_some() {
                    "\u{2611}"
                } else {
                    "\u{2610}"
                }
            ));
        }
        // Tight list items hold paragraphs; drop their <p> so items don't
        // get paragraph spacing.
        for child in node.children() {
            if tight && matches!(child.data().value, NodeValue::Paragraph) {
                self.children(child);
            } else {
                self.node(child);
            }
        }
        self.out.push_str("</li>");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn opts() -> RenderOptions {
        RenderOptions {
            breaks: false,
            linkify: true,
            typographer: false,
            math: true,
            front_matter: 2,
        }
    }

    #[test]
    fn splits_blocks_with_lines() {
        let doc = "# Title\n\nPara *em*\nnext line\n\n```python\nx = 1\n```\n\n$$\na^2\n$$\n";
        let b = blocks(doc, &opts());
        let summary: Vec<_> = b
            .iter()
            .map(|b| (b.kind, b.first_line, b.last_line))
            .collect();
        assert_eq!(
            summary,
            vec![
                (BlockKind::Html, 0, 0),
                (BlockKind::Html, 2, 3),
                (BlockKind::Code, 5, 7),
                (BlockKind::Math, 9, 11)
            ]
        );
        assert!(
            b[0].html
                .starts_with("<h1><a name=\"title\"></a>Title</h1>")
        );
        assert_eq!(b[1].html, "<p>Para <em>em</em>\nnext line</p>");
        assert_eq!(b[2].lang, "python");
        assert_eq!(b[2].text, "x = 1");
        assert_eq!(b[3].text, "a^2");
    }

    #[test]
    fn references_tasks_and_inline_math() {
        let doc = "- [x] done [site][s]\n- [ ] todo $x^2$\n\n[s]: https://example.com \"Ex\"\n";
        let b = blocks(doc, &opts());
        assert_eq!(b.len(), 2);
        assert!(b[0].html.contains("list-style-type: none"));
        assert!(
            b[0].html
                .contains("<a class=\"task\" href=\"vn-task:0\">\u{2611}</a>")
        );
        assert!(
            b[0].html
                .contains("<a href=\"https://example.com\" title=\"Ex\">site</a>")
        );
        assert!(b[0].html.contains("vn-math:0"));
        assert_eq!(b[0].inline_math, vec!["x^2".to_string()]);
        assert_eq!(b[1].kind, BlockKind::Hidden);
    }

    #[test]
    fn front_matter_modes() {
        let doc = "---\ntitle: Notes\ntags: x\n---\n\nText\n";
        let mut o = opts();
        let b = blocks(doc, &o);
        assert_eq!(b[0].kind, BlockKind::Html);
        assert!(
            b[0].html
                .contains("<th><a name=\"vn-line:1\"></a>title</th><td>Notes</td>")
        );
        assert!(
            b[0].html
                .contains("<th><a name=\"vn-line:2\"></a>tags</th>")
        );
        assert_eq!((b[0].first_line, b[0].last_line), (0, 3));
        o.front_matter = 1;
        assert_eq!(blocks(doc, &o)[0].kind, BlockKind::FrontMatter);
        o.front_matter = 0;
        assert_eq!(blocks(doc, &o)[0].kind, BlockKind::Hidden);
    }

    #[test]
    fn options_change_output() {
        let mut o = opts();
        o.breaks = true;
        o.typographer = true;
        let b = blocks("\"Hi\" -- a\nb\n", &o);
        assert_eq!(b[0].html, "<p>\u{201C}Hi\u{201D} \u{2013} a<br />b</p>");
        o.linkify = false;
        assert!(!blocks("see www.example.com\n", &o)[0].html.contains("<a"));
        o.linkify = true;
        assert!(
            blocks("see www.example.com\n", &o)[0]
                .html
                .contains("<a href")
        );
    }

    #[test]
    fn table_alignment_and_escaping() {
        let b = blocks("| a | b |\n|:--|--:|\n| 1 < 2 | 2 |\n", &opts());
        assert!(b[0].html.contains("<th align=\"left\">a</th>"));
        assert!(
            b[0].html
                .contains("<td align=\"left\"><a name=\"vn-line:2\"></a>1 &lt; 2</td>")
        );
        assert!(b[0].html.contains("<td align=\"right\">2</td>"));
    }

    fn lines(doc: &str) -> Vec<(BlockKind, usize, usize)> {
        blocks(doc, &opts())
            .iter()
            .map(|b| (b.kind, b.first_line, b.last_line))
            .collect()
    }

    #[test]
    fn definitions_become_hidden_blocks() {
        let doc = "text\n\n[a]: x.md\n[b]: <y z.md>\n   \"multi\n   line\"\n[c]:\n  w.md 'T'\n\nafter [a]\n";
        assert_eq!(
            lines(doc),
            vec![
                (BlockKind::Html, 0, 0),
                (BlockKind::Hidden, 2, 2),
                (BlockKind::Hidden, 3, 5),
                (BlockKind::Hidden, 6, 7),
                (BlockKind::Html, 9, 9),
            ]
        );
        assert!(
            blocks(doc, &opts())[4]
                .html
                .contains("<a href=\"x.md\">a</a>")
        );
    }

    #[test]
    fn text_that_is_not_a_definition_stays_text() {
        let b = blocks("[a]: x.md trailing\n\npara\n[b]: y\n", &opts());
        assert!(b.iter().all(|b| b.kind == BlockKind::Html));
        assert_eq!(b.len(), 2);
    }

    #[test]
    fn footnotes_keep_source_order_and_numbers() {
        let doc = "One[^b] two[^a]\n\n[^a]: Note A\n\n[^b]: Note B\n\nLast\n";
        let b = blocks(doc, &opts());
        let kinds: Vec<_> = b.iter().map(|b| b.first_line).collect();
        assert_eq!(kinds, vec![0, 2, 4, 6]);
        assert!(b[0].html.contains("<sup><a href=\"#fn-b\">2</a></sup>"));
        assert!(
            b[1].html
                .starts_with("<div class=\"footnote\"><a name=\"fn-a\"></a><sup>1</sup> ")
        );
    }

    #[test]
    fn nested_display_math_and_tight_lists() {
        let b = blocks("- a\n- b\n\n> $$\n> x\n> $$\n", &opts());
        assert_eq!(
            b[0].html,
            "<ul><li>a</li><li><a name=\"vn-line:1\"></a>b</li></ul>"
        );
        assert!(b[1].html.contains("vn-math-display:0"));
        assert_eq!(b[1].inline_math, vec!["x".to_string()]);
    }

    fn markers(html: &str) -> Vec<usize> {
        html.split("<a name=\"vn-line:")
            .skip(1)
            .map(|s| s[..s.find('"').unwrap()].parse().unwrap())
            .collect()
    }

    #[test]
    fn line_markers_are_relative_to_the_block() {
        // Table: body rows, after the header (0) and delimiter (1) lines.
        let b = blocks("intro\n\n| h |\n|---|\n| r1 |\n| r2 |\n", &opts());
        assert_eq!(markers(&b[1].html), vec![2, 3]);
        assert!(
            b[1].html
                .starts_with("<table cellspacing=\"0\" cellpadding=\"5\"><tr><th>h</th>")
        );
        // Lists, nested items and paragraphs in a quote.
        let b = blocks("x\n\n- a\n  - b\n- c\n\n> p\n>\n> q\n", &opts());
        assert_eq!(markers(&b[1].html), vec![1, 2]);
        assert_eq!(markers(&b[2].html), vec![2]);
        // A one-line block has none.
        assert!(markers(&blocks("# T\n", &opts())[0].html).is_empty());
    }

    #[test]
    fn duplicate_headings_get_suffixes() {
        let b = blocks("# A\n\n# A\n", &opts());
        assert!(b[1].html.contains("<a name=\"a-1\"></a>"));
    }

    #[test]
    fn front_matter_text_is_the_yaml_body() {
        let mut o = opts();
        o.front_matter = 1;
        let b = blocks("---\ntitle: x\n---\n\ntext\n", &o);
        assert_eq!(b[0].text, "title: x");
        assert_eq!((b[0].first_line, b[0].last_line), (0, 2));
    }

    #[test]
    fn crlf_line_ranges() {
        assert_eq!(
            lines("# a\r\n\r\n[x]: y\r\n\r\ntext\r\n"),
            vec![
                (BlockKind::Html, 0, 0),
                (BlockKind::Hidden, 2, 2),
                (BlockKind::Html, 4, 4)
            ]
        );
    }

    #[test]
    fn perf_two_megabyte_document() {
        let unit = "## Heading\n\nA paragraph with *emphasis*, a [link](https://example.com/page) and `code`.\n\n- first item\n- second item\n\n\n";
        let doc = unit.repeat(20_000);
        assert!(doc.len() > 1_900_000);
        let start = std::time::Instant::now();
        let b = blocks(&doc, &opts());
        let elapsed = start.elapsed();
        println!(
            "blocks() on {} bytes: {elapsed:?} ({} blocks)",
            doc.len(),
            b.len()
        );
        assert_eq!(b.len(), 60_000);
        // A guard against quadratic blowups, not a benchmark: shared CI
        // runners took 3.3 s for a debug build that usually takes under 2 s.
        assert!(
            elapsed < std::time::Duration::from_secs(10),
            "took {elapsed:?}"
        );
    }
}
