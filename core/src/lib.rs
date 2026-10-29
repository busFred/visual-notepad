//! Rust side of Visual Notepad: Markdown parsing (markdown-rs), later Mermaid
//! rendering. Exposed to C++ through the cxx bridge below.

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

    extern "Rust" {
        fn outline(markdown: &str) -> Vec<Heading>;
    }
}

/// GFM plus math and front matter, matching what the VS Code Markdown
/// editor accepted.
fn parse_options() -> ParseOptions {
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
