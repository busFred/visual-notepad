//! Rust side of Visual Notepad: Markdown parsing and rendering (markdown-rs)
//! and Mermaid diagrams. Exposed to C++ through the cxx bridge below.

mod format;
mod links;
mod mermaid;
mod render;

use markdown::mdast::Node;
use markdown::{Constructs, ParseOptions};

#[cxx::bridge(namespace = "vn::core")]
mod ffi {
    /// One heading in the document outline.
    struct Heading {
        /// 1 to 6.
        level: u8,
        /// Plain text of the heading, inline markup stripped.
        text: String,
        /// 1-based source line of the heading's first character.
        line: usize,
    }

    /// How a top-level block is shown in the visual editor.
    #[derive(Debug)]
    enum BlockKind {
        /// Rendered from `html`.
        Html,
        /// Fenced or indented code; `lang` and `text`.
        Code,
        /// Display math; TeX in `text`.
        Math,
        /// Front matter shown as code; source in `text`.
        FrontMatter,
        /// Not shown (link definitions, hidden front matter).
        Hidden,
    }

    /// One top-level block with its 0-based source line range.
    #[derive(Debug)]
    struct Block {
        kind: BlockKind,
        first_line: usize,
        last_line: usize,
        /// For Html blocks. Inline math appears as <img src="vn-math:N">
        /// (and display math nested in lists as "vn-math-display:N").
        html: String,
        lang: String,
        text: String,
        /// TeX for each vn-math image, by index.
        inline_math: Vec<String>,
    }

    /// Markdown rendering settings from Preferences.
    struct RenderOptions {
        breaks: bool,
        linkify: bool,
        typographer: bool,
        math: bool,
        /// 0 = hide, 1 = code block, 2 = table.
        front_matter: u8,
    }

    /// A link, image or definition destination in the source text.
    struct LinkTarget {
        /// UTF-16 offset of the destination's first character.
        start: usize,
        /// UTF-16 offset just past the destination's last character.
        end: usize,
        /// The destination as written (percent-encoding intact).
        dest: String,
    }

    extern "Rust" {
        fn outline(markdown: &str) -> Vec<Heading>;
        fn blocks(markdown: &str, options: &RenderOptions) -> Vec<Block>;
        /// PNG bytes of a Mermaid diagram; throws with the parse error.
        fn mermaid_png(code: &str, theme: &str, scale: f32) -> Result<Vec<u8>>;
        /// GitHub-style anchor for a heading's text.
        fn heading_slug(text: &str) -> String;
        /// Canonical CommonMark form of a document (GFM, front matter, math).
        fn format_markdown(markdown: &str) -> String;
        /// Rewrites relative destinations so that a text copied from a
        /// document in `from_dir` keeps pointing at the same files when pasted
        /// into a document in `to_dir`.
        fn rebase_links(text: &str, from_dir: &str, to_dir: &str) -> String;
        /// Destinations of inline links, images and definitions, in order.
        fn link_targets(markdown: &str) -> Vec<LinkTarget>;
    }
}

fn blocks(markdown: &str, options: &ffi::RenderOptions) -> Vec<ffi::Block> {
    render::blocks(markdown, options)
}

fn mermaid_png(code: &str, theme: &str, scale: f32) -> Result<Vec<u8>, String> {
    mermaid::png(code, theme, scale)
}

fn heading_slug(text: &str) -> String {
    render::slug(text)
}

fn format_markdown(markdown: &str) -> String {
    format::format(markdown)
}

fn rebase_links(text: &str, from_dir: &str, to_dir: &str) -> String {
    links::rebase(text, from_dir, to_dir)
}

fn link_targets(markdown: &str) -> Vec<ffi::LinkTarget> {
    links::targets(markdown)
}

/// GFM plus math and front matter, matching what the VS Code Markdown
/// editor accepted.
pub(crate) fn parse_options() -> ParseOptions {
    ParseOptions {
        constructs: Constructs {
            frontmatter: true,
            math_flow: true,
            math_text: true,
            ..Constructs::gfm()
        },
        ..ParseOptions::gfm()
    }
}

/// Headings in document order. Returns an empty list if parsing fails, which
/// cannot happen without MDX constructs enabled.
pub fn outline(markdown: &str) -> Vec<ffi::Heading> {
    let mut headings = Vec::new();
    if let Ok(root) = markdown::to_mdast(markdown, &parse_options()) {
        collect_headings(&root, &mut headings);
    }
    headings
}

fn collect_headings(node: &Node, out: &mut Vec<ffi::Heading>) {
    if let Node::Heading(heading) = node {
        out.push(ffi::Heading {
            level: heading.depth,
            text: node.to_string(),
            line: heading.position.as_ref().map_or(0, |p| p.start.line),
        });
        return;
    }
    if let Some(children) = node.children() {
        for child in children {
            collect_headings(child, out);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn outline_lists_headings_with_lines() {
        let doc = "---\ntitle: x\n---\n# One\n\ntext\n\n## Two *em*\n\n> # Quoted\n\n```\n# not a heading\n```\n";
        let got: Vec<_> = outline(doc)
            .into_iter()
            .map(|h| (h.level, h.text, h.line))
            .collect();
        assert_eq!(
            got,
            vec![
                (1, "One".to_string(), 4),
                (2, "Two em".to_string(), 8),
                (1, "Quoted".to_string(), 10),
            ]
        );
    }

    #[test]
    fn outline_of_plain_text_is_empty() {
        assert!(outline("just text\n").is_empty());
    }
}
