//! Rust side of Visual Notepad: Markdown parsing and rendering (comrak)
//! and Mermaid diagrams. Exposed to C++ through the cxx bridge below.

mod format;
mod links;
mod md;
mod mermaid;
mod render;

use comrak::nodes::{AstNode, NodeValue};
use comrak::{Arena, parse_document};

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

    /// A raster image: premultiplied RGBA8, `width * height * 4` bytes.
    struct RgbaImage {
        width: u32,
        height: u32,
        pixels: Vec<u8>,
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
        /// Premultiplied RGBA pixels of a Mermaid diagram; throws with the parse error.
        fn mermaid_image(code: &str, theme: &str, scale: f32) -> Result<RgbaImage>;
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

fn mermaid_image(code: &str, theme: &str, scale: f32) -> Result<ffi::RgbaImage, String> {
    let (width, height, pixels) = mermaid::raster(code, theme, scale)?;
    Ok(ffi::RgbaImage { width, height, pixels })
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

/// Headings in document order.
pub fn outline(markdown: &str) -> Vec<ffi::Heading> {
    let arena = Arena::new();
    let root = parse_document(&arena, markdown, &md::options(true, false));
    root.descendants().filter_map(heading).collect()
}

fn heading<'a>(node: &'a AstNode<'a>) -> Option<ffi::Heading> {
    let data = node.data();
    let NodeValue::Heading(h) = &data.value else {
        return None;
    };
    let mut text = String::new();
    md::plain_text(node, &mut text);
    Some(ffi::Heading {
        level: h.level,
        text,
        line: data.sourcepos.start.line,
    })
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
