//! Splits a Markdown document into top-level blocks with their source lines
//! and renders each block to HTML that Qt's rich text engine understands.
//! Code blocks, display math and front matter are returned as raw text so
//! the C++ side can draw them (syntax highlighting, math, diagrams).

use std::collections::HashMap;

use markdown::mdast::{AlignKind, Node};
use markdown::{Constructs, ParseOptions};

use crate::ffi::{Block, BlockKind, RenderOptions};

pub fn parse_options(options: &RenderOptions) -> ParseOptions {
    ParseOptions {
        constructs: Constructs {
            frontmatter: true,
            math_flow: options.math,
            math_text: options.math,
            gfm_autolink_literal: options.linkify,
            ..Constructs::gfm()
        },
        ..ParseOptions::gfm()
    }
}

pub fn blocks(markdown: &str, options: &RenderOptions) -> Vec<Block> {
    let Ok(root) = markdown::to_mdast(markdown, &parse_options(options)) else {
        return Vec::new();
    };
    let Some(children) = root.children() else {
        return Vec::new();
    };

    let mut definitions = HashMap::new();
    let mut footnotes = Vec::new();
    collect_definitions(&root, &mut definitions, &mut footnotes);
    let mut renderer = Html {
        options,
        definitions: &definitions,
        footnotes: &footnotes,
        out: String::new(),
        inline_math: Vec::new(),
        slugs: HashMap::new(),
    };

    let mut result = Vec::new();
    for node in children {
        let Some(position) = node.position() else {
            continue;
        };
        let first_line = position.start.line - 1;
        // The end point is the first character after the node.
        let last_line = if position.end.column == 1 && position.end.line > position.start.line {
            position.end.line - 2
        } else {
            position.end.line - 1
        };
        let mut block = Block {
            kind: BlockKind::Html,
            first_line,
            last_line,
            html: String::new(),
            lang: String::new(),
            text: String::new(),
            inline_math: Vec::new(),
        };
        match node {
            Node::Code(code) => {
                block.kind = BlockKind::Code;
                block.lang = code.lang.clone().unwrap_or_default();
                block.text = code.value.clone();
            }
            Node::Math(math) => {
                block.kind = BlockKind::Math;
                block.text = math.value.clone();
            }
            Node::Yaml(yaml) => front_matter(&mut block, &yaml.value, options),
            Node::Toml(toml) => front_matter(&mut block, &toml.value, options),
            Node::Definition(_) => block.kind = BlockKind::Hidden,
            _ => {
                renderer.out.clear();
                renderer.inline_math.clear();
                renderer.node(node);
                block.html = std::mem::take(&mut renderer.out);
                block.inline_math = std::mem::take(&mut renderer.inline_math);
            }
        }
        result.push(block);
    }
    result
}

fn front_matter(block: &mut Block, value: &str, options: &RenderOptions) {
    block.text = value.to_string();
    match options.front_matter {
        0 => block.kind = BlockKind::Hidden,
        2 => {
            // Simple "key: value" lines become a table; anything nested is
            // shown as code instead.
            let mut rows = String::new();
            for line in value.lines().filter(|l| !l.trim().is_empty()) {
                let Some((key, val)) = line.split_once(':') else {
                    block.kind = BlockKind::FrontMatter;
                    return;
                };
                if line.starts_with([' ', '\t', '-']) {
                    block.kind = BlockKind::FrontMatter;
                    return;
                }
                rows.push_str(&format!(
                    "<tr><th>{}</th><td>{}</td></tr>",
                    escape(key.trim()),
                    escape(val.trim().trim_matches('"'))
                ));
            }
            block.kind = BlockKind::Html;
            block.html = format!("<table class=\"frontmatter\" cellspacing=\"0\" cellpadding=\"4\">{rows}</table>");
        }
        _ => block.kind = BlockKind::FrontMatter,
    }
}

fn collect_definitions<'a>(
    node: &'a Node,
    definitions: &mut HashMap<String, (String, Option<String>)>,
    footnotes: &mut Vec<String>,
) {
    match node {
        Node::Definition(d) => {
            definitions
                .entry(d.identifier.to_lowercase())
                .or_insert((d.url.clone(), d.title.clone()));
        }
        Node::FootnoteDefinition(f) => footnotes.push(f.identifier.to_lowercase()),
        _ => {}
    }
    if let Some(children) = node.children() {
        for child in children {
            collect_definitions(child, definitions, footnotes);
        }
    }
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
    let text = text.replace("---", "\u{2014}").replace("--", "\u{2013}").replace("...", "\u{2026}");
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
    definitions: &'a HashMap<String, (String, Option<String>)>,
    footnotes: &'a [String],
    out: String,
    inline_math: Vec<String>,
    slugs: HashMap<String, usize>,
}

impl Html<'_> {
    fn children(&mut self, node: &Node) {
        if let Some(children) = node.children() {
            for child in children {
                self.node(child);
            }
        }
    }

    fn text(&mut self, value: &str) {
        let value = if self.options.typographer { typographer(value) } else { value.to_string() };
        let escaped = escape(&value);
        if self.options.breaks {
            self.out.push_str(&escaped.replace('\n', "<br />"));
        } else {
            self.out.push_str(&escaped);
        }
    }

    fn link_open(&mut self, url: &str, title: Option<&String>) {
        self.out.push_str(&format!("<a href=\"{}\"", escape(url)));
        if let Some(title) = title {
            self.out.push_str(&format!(" title=\"{}\"", escape(title)));
        }
        self.out.push('>');
    }

    fn image(&mut self, url: &str, alt: &str, title: Option<&String>) {
        self.out.push_str(&format!("<img src=\"{}\" alt=\"{}\"", escape(url), escape(alt)));
        if let Some(title) = title {
            self.out.push_str(&format!(" title=\"{}\"", escape(title)));
        }
        self.out.push_str(" />");
    }

    fn node(&mut self, node: &Node) {
        match node {
            Node::Root(_) => self.children(node),
            Node::Paragraph(_) => {
                self.out.push_str("<p>");
                self.children(node);
                self.out.push_str("</p>");
            }
            Node::Heading(h) => {
                let mut slug = slug(&node.to_string());
                let count = self.slugs.entry(slug.clone()).or_insert(0);
                if *count > 0 {
                    slug = format!("{slug}-{count}");
                }
                *count += 1;
                self.out.push_str(&format!("<h{d}><a name=\"{}\"></a>", escape(&slug), d = h.depth));
                self.children(node);
                self.out.push_str(&format!("</h{}>", h.depth));
            }
            Node::Text(t) => self.text(&t.value),
            Node::Emphasis(_) => {
                self.out.push_str("<em>");
                self.children(node);
                self.out.push_str("</em>");
            }
            Node::Strong(_) => {
                self.out.push_str("<strong>");
                self.children(node);
                self.out.push_str("</strong>");
            }
            Node::Delete(_) => {
                self.out.push_str("<s>");
                self.children(node);
                self.out.push_str("</s>");
            }
            Node::InlineCode(c) => {
                self.out.push_str(&format!("<code>{}</code>", escape(&c.value)));
            }
            Node::InlineMath(m) => {
                self.out.push_str(&format!(
                    "<img src=\"vn-math:{}\" style=\"vertical-align: middle\" />",
                    self.inline_math.len()
                ));
                self.inline_math.push(m.value.clone());
            }
            Node::Math(m) => {
                // Display math nested in a list or quote.
                self.out.push_str(&format!(
                    "<p align=\"center\"><img src=\"vn-math-display:{}\" /></p>",
                    self.inline_math.len()
                ));
                self.inline_math.push(m.value.clone());
            }
            Node::Break(_) => self.out.push_str("<br />"),
            Node::Link(l) => {
                self.link_open(&l.url, l.title.as_ref());
                self.children(node);
                self.out.push_str("</a>");
            }
            Node::LinkReference(r) => match self.definitions.get(&r.identifier.to_lowercase()).cloned() {
                Some((url, title)) => {
                    self.link_open(&url, title.as_ref());
                    self.children(node);
                    self.out.push_str("</a>");
                }
                None => self.children(node),
            },
            Node::Image(i) => self.image(&i.url, &i.alt, i.title.as_ref()),
            Node::ImageReference(r) => {
                if let Some((url, title)) = self.definitions.get(&r.identifier.to_lowercase()).cloned() {
                    self.image(&url, &r.alt, title.as_ref());
                } else {
                    self.text(&r.alt);
                }
            }
            Node::FootnoteReference(f) => {
                let id = f.identifier.to_lowercase();
                let n = self.footnotes.iter().position(|x| *x == id).map_or(0, |i| i + 1);
                self.out.push_str(&format!(
                    "<sup><a href=\"#fn-{}\">{}</a></sup>",
                    escape(&slug(&id)),
                    if n > 0 { n.to_string() } else { escape(&id) }
                ));
            }
            Node::FootnoteDefinition(f) => {
                let id = f.identifier.to_lowercase();
                let n = self.footnotes.iter().position(|x| *x == id).map_or(0, |i| i + 1);
                self.out.push_str(&format!(
                    "<div class=\"footnote\"><a name=\"fn-{}\"></a><sup>{}</sup> ",
                    escape(&slug(&id)),
                    n
                ));
                self.children(node);
                self.out.push_str("</div>");
            }
            Node::Blockquote(_) => {
                self.out.push_str("<blockquote>");
                self.children(node);
                self.out.push_str("</blockquote>");
            }
            Node::List(l) => {
                let tasks = node
                    .children()
                    .is_some_and(|items| items.iter().all(|i| matches!(i, Node::ListItem(li) if li.checked.is_some())));
                let style = if tasks { " style=\"list-style-type: none\"" } else { "" };
                if l.ordered {
                    let start = l.start.unwrap_or(1);
                    self.out.push_str(&format!("<ol start=\"{start}\"{style}>"));
                } else {
                    self.out.push_str(&format!("<ul{style}>"));
                }
                self.children(node);
                self.out.push_str(if l.ordered { "</ol>" } else { "</ul>" });
            }
            Node::ListItem(item) => {
                self.out.push_str("<li>");
                if let Some(checked) = item.checked {
                    let line = node.position().map_or(0, |p| p.start.line - 1);
                    self.out.push_str(&format!(
                        "<a class=\"task\" href=\"vn-task:{line}\">{}</a> ",
                        if checked { "\u{2611}" } else { "\u{2610}" }
                    ));
                }
                // Tight list items hold paragraphs; drop their <p> so items
                // don't get paragraph spacing.
                if let Some(children) = node.children() {
                    for child in children {
                        if let (Node::Paragraph(_), false) = (child, item.spread) {
                            self.children(child);
                        } else {
                            self.node(child);
                        }
                    }
                }
                self.out.push_str("</li>");
            }
            Node::Code(c) => {
                self.out.push_str(&format!("<pre>{}</pre>", escape(c.value.trim_end_matches('\n'))));
            }
            Node::Html(h) => self.out.push_str(&h.value),
            Node::ThematicBreak(_) => self.out.push_str("<hr />"),
            Node::Table(t) => {
                self.out.push_str("<table cellspacing=\"0\" cellpadding=\"5\">");
                if let Some(rows) = node.children() {
                    for (r, row) in rows.iter().enumerate() {
                        self.out.push_str("<tr>");
                        if let Some(cells) = row.children() {
                            for (c, cell) in cells.iter().enumerate() {
                                let tag = if r == 0 { "th" } else { "td" };
                                let align = match t.align.get(c) {
                                    Some(AlignKind::Left) => " align=\"left\"",
                                    Some(AlignKind::Right) => " align=\"right\"",
                                    Some(AlignKind::Center) => " align=\"center\"",
                                    _ => "",
                                };
                                self.out.push_str(&format!("<{tag}{align}>"));
                                self.children(cell);
                                self.out.push_str(&format!("</{tag}>"));
                            }
                        }
                        self.out.push_str("</tr>");
                    }
                }
                self.out.push_str("</table>");
            }
            Node::Definition(_) | Node::Yaml(_) | Node::Toml(_) => {}
            _ => self.children(node),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn opts() -> RenderOptions {
        RenderOptions { breaks: false, linkify: true, typographer: false, math: true, front_matter: 2 }
    }

    #[test]
    fn splits_blocks_with_lines() {
        let doc = "# Title\n\nPara *em*\nnext line\n\n```python\nx = 1\n```\n\n$$\na^2\n$$\n";
        let b = blocks(doc, &opts());
        let summary: Vec<_> = b.iter().map(|b| (b.kind, b.first_line, b.last_line)).collect();
        assert_eq!(
            summary,
            vec![(BlockKind::Html, 0, 0), (BlockKind::Html, 2, 3), (BlockKind::Code, 5, 7), (BlockKind::Math, 9, 11)]
        );
        assert!(b[0].html.starts_with("<h1><a name=\"title\"></a>Title</h1>"));
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
        assert!(b[0].html.contains("<a class=\"task\" href=\"vn-task:0\">\u{2611}</a>"));
        assert!(b[0].html.contains("<a href=\"https://example.com\" title=\"Ex\">site</a>"));
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
        assert!(b[0].html.contains("<th>title</th><td>Notes</td>"));
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
        assert!(blocks("see www.example.com\n", &o)[0].html.contains("<a href"));
    }

    #[test]
    fn table_alignment_and_escaping() {
        let b = blocks("| a | b |\n|:--|--:|\n| 1 < 2 | 2 |\n", &opts());
        assert!(b[0].html.contains("<th align=\"left\">a</th>"));
        assert!(b[0].html.contains("<td align=\"left\">1 &lt; 2</td>"));
        assert!(b[0].html.contains("<td align=\"right\">2</td>"));
    }
}
