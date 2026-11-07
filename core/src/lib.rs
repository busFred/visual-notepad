//! Rust side of Visual Notepad: Markdown parsing and rendering (comrak)
//! and Mermaid diagrams. Exposed to C++ through the cxx bridge below.

mod format;
mod gzip;
mod html_table;
mod links;
mod md;
mod mermaid;
mod render;
mod table;

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
    #[derive(Clone, Debug, PartialEq)]
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
        /// Device pixel ratio the pixels were drawn at (may be below the one asked for).
        scale: f32,
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

    /// What can be done to a table (see `table_edit`).
    #[derive(Debug, PartialEq)]
    enum TableOp {
        RowAbove,
        RowBelow,
        ColumnLeft,
        ColumnRight,
        DeleteRow,
        DeleteColumn,
        DeleteTable,
        RowUp,
        RowDown,
        ColumnBefore,
        ColumnAfter,
        AlignNone,
        AlignLeft,
        AlignCenter,
        AlignRight,
        SortAscending,
        SortDescending,
        Transpose,
        Realign,
        /// Replaces the text of one cell with the `arg` given.
        SetCell,
        ToHtml,
        ToPipe,
    }

    /// The table around a position. Rows count from the header (0); the
    /// delimiter line is part of the header.
    struct TableInfo {
        found: bool,
        /// 0-based lines, the last one included.
        first_line: usize,
        last_line: usize,
        /// Including the header.
        rows: usize,
        cols: usize,
        /// The row and column of the position.
        row: usize,
        col: usize,
        /// Of that column: 0 none, 1 left, 2 center, 3 right.
        alignment: u8,
        /// Whether the text is what `Realign` would make of it.
        aligned: bool,
        /// Whether this is an HTML table.
        html: bool,
        /// Bit `1 << TableOp.repr` for accepted operations (except aligned Realign).
        available: u32,
    }

    /// Lines to put in place of `first_line..=last_line`.
    struct TableEdit {
        ok: bool,
        changed: bool,
        /// The lines go and nothing replaces them (`replacement` is empty).
        remove: bool,
        first_line: usize,
        last_line: usize,
        replacement: String,
        /// Where the caret goes, in the lines after the edit.
        caret_line: usize,
        caret_column: usize,
        /// Lines at the start and at the end of the old lines that are the
        /// same in `replacement`: only the ones between need replacing.
        keep_before: usize,
        keep_after: usize,
    }

    /// The text of one cell, possibly spanning multiple lines.
    struct TableCell {
        found: bool,
        line: usize,
        /// UTF-16 columns on `line` and `end_line`.
        start: usize,
        end_line: usize,
        end: usize,
        text: String,
    }

    extern "Rust" {
        /// The table that holds 0-based `line` of `text`, and which cell
        /// `column` (UTF-16 units into the line) is in.
        fn table_info(text: &str, line: usize, column: usize) -> TableInfo;
        /// The table holding `line` after `op` on the cell at (`row`, `col`).
        fn table_edit(text: &str, line: usize, op: TableOp, row: usize, col: usize, arg: &str) -> TableEdit;
        /// `table_edit` for the `rows` x `cols` cells starting at (`row`,
        /// `col`): see `table::edit_range`.
        #[allow(clippy::too_many_arguments)]
        fn table_edit_range(
            text: &str,
            line: usize,
            op: TableOp,
            row: usize,
            col: usize,
            rows: usize,
            cols: usize,
            arg: &str,
        ) -> TableEdit;
        /// Where cell (`row`, `col`) of the table holding `line` is.
        fn table_cell(text: &str, line: usize, row: usize, col: usize) -> TableCell;
        fn outline(markdown: &str) -> Vec<Heading>;
        fn blocks(markdown: &str, options: &RenderOptions) -> Vec<Block>;
        /// A document's blocks together with what it takes to parse the
        /// next version of its text incrementally.
        type Document;
        /// The same blocks as `blocks`.
        fn parse(markdown: &str, options: &RenderOptions) -> Box<Document>;
        /// The blocks of `markdown`, a new version of `previous`'s text:
        /// exactly those of `parse(markdown, options)`, made by parsing only
        /// the blocks around the edits when that is safe.
        fn reparse(previous: &Document, markdown: &str, options: &RenderOptions) -> Box<Document>;
        fn blocks(self: &Document) -> &Vec<Block>;
        /// Whether `reparse` parsed only the blocks around the edits.
        fn spliced(self: &Document) -> bool;
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
        /// Contents of gzip data (all members), or nothing if it is not valid
        /// gzip or longer than `limit` bytes.
        fn gunzip_bounded(data: &[u8], limit: usize) -> Vec<u8>;
    }
}

// A panic must not reach the bridge: cxx aborts the process there, and the
// session restore would reopen the same document and abort again. Every
// bridged function runs its body through `guarded` and gives a neutral
// result instead (the document shown as plain text, nothing found, the text
// unchanged, an error). Document::blocks and Document::spliced, the other
// two, only read a field.

/// `f()`, or `fallback()` if `f` panics.
fn guarded<T>(f: impl FnOnce() -> T, fallback: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or_else(|_| fallback())
}

/// The whole text as one plain code block: the document is still shown
/// (and editable) when parsing it panicked.
fn unparsed_blocks(markdown: &str) -> Vec<ffi::Block> {
    if markdown.is_empty() {
        return Vec::new();
    }
    vec![ffi::Block {
        kind: ffi::BlockKind::Code,
        first_line: 0,
        last_line: markdown.lines().count().saturating_sub(1),
        html: String::new(),
        lang: String::new(),
        text: markdown.to_owned(),
        inline_math: Vec::new(),
    }]
}

fn gunzip_bounded(data: &[u8], limit: usize) -> Vec<u8> {
    guarded(|| gzip::gunzip_bounded(data, limit), Vec::new)
}

fn blocks(markdown: &str, options: &ffi::RenderOptions) -> Vec<ffi::Block> {
    guarded(|| render::blocks(markdown, options), || unparsed_blocks(markdown))
}

use render::Document;

fn parse(markdown: &str, options: &ffi::RenderOptions) -> Box<Document> {
    guarded(
        || Box::new(Document::parse_large(markdown, options)),
        || Box::new(Document::unparsed(markdown, unparsed_blocks(markdown))),
    )
}

fn reparse(previous: &Document, markdown: &str, options: &ffi::RenderOptions) -> Box<Document> {
    guarded(
        || Box::new(render::reparse(previous, markdown, options)),
        || parse(markdown, options),
    )
}

fn mermaid_image(code: &str, theme: &str, scale: f32) -> Result<ffi::RgbaImage, String> {
    guarded(
        || {
            let (width, height, pixels, scale) = mermaid::raster(code, theme, scale)?;
            Ok(ffi::RgbaImage { width, height, pixels, scale })
        },
        || Err("The diagram renderer failed.".to_owned()),
    )
}

fn heading_slug(text: &str) -> String {
    guarded(|| render::slug(text), String::new)
}

fn format_markdown(markdown: &str) -> String {
    guarded(|| format::format(markdown), || markdown.to_owned())
}

fn rebase_links(text: &str, from_dir: &str, to_dir: &str) -> String {
    guarded(|| links::rebase(text, from_dir, to_dir), || text.to_owned())
}

fn link_targets(markdown: &str) -> Vec<ffi::LinkTarget> {
    guarded(|| links::targets(markdown), Vec::new)
}

fn table_info(text: &str, line: usize, column: usize) -> ffi::TableInfo {
    guarded(
        || table::info(text, line, column),
        || ffi::TableInfo {
            found: false,
            first_line: 0,
            last_line: 0,
            rows: 0,
            cols: 0,
            row: 0,
            col: 0,
            alignment: 0,
            aligned: true,
            html: false,
            available: 0,
        },
    )
}

fn table_edit(text: &str, line: usize, op: ffi::TableOp, row: usize, col: usize, arg: &str) -> ffi::TableEdit {
    guarded(
        || table::edit(text, line, op, row, col, arg),
        || ffi::TableEdit {
            ok: false,
            changed: false,
            remove: false,
            first_line: 0,
            last_line: 0,
            replacement: String::new(),
            caret_line: 0,
            caret_column: 0,
            keep_before: 0,
            keep_after: 0,
        },
    )
}

#[allow(clippy::too_many_arguments)]
fn table_edit_range(
    text: &str,
    line: usize,
    op: ffi::TableOp,
    row: usize,
    col: usize,
    rows: usize,
    cols: usize,
    arg: &str,
) -> ffi::TableEdit {
    guarded(
        || table::edit_range(text, line, op, row, col, rows, cols, arg),
        || ffi::TableEdit {
            ok: false,
            changed: false,
            remove: false,
            first_line: 0,
            last_line: 0,
            replacement: String::new(),
            caret_line: 0,
            caret_column: 0,
            keep_before: 0,
            keep_after: 0,
        },
    )
}

fn table_cell(text: &str, line: usize, row: usize, col: usize) -> ffi::TableCell {
    guarded(
        || table::cell(text, line, row, col),
        || ffi::TableCell { found: false, line: 0, start: 0, end_line: 0, end: 0, text: String::new() },
    )
}

fn outline(markdown: &str) -> Vec<ffi::Heading> {
    guarded(|| headings(markdown), Vec::new)
}

/// Headings in document order.
pub fn headings(markdown: &str) -> Vec<ffi::Heading> {
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

    #[test]
    fn panics_give_the_fallback() {
        assert_eq!(guarded(|| -> Vec<u8> { panic!("deliberate") }, Vec::new), Vec::<u8>::new());
        assert_eq!(guarded(|| 1, || 2), 1);
        let text = "# One\n\ntext";
        assert_eq!(
            guarded(|| -> String { panic!("deliberate") }, || text.to_owned()),
            text
        );
        // What parse and reparse are left with.
        let blocks = unparsed_blocks(text);
        assert_eq!(blocks.len(), 1);
        assert_eq!(blocks[0].kind, ffi::BlockKind::Code);
        assert_eq!((blocks[0].first_line, blocks[0].last_line), (0, 2));
        assert_eq!(blocks[0].text, text);
        assert!(unparsed_blocks("").is_empty());
        let options = ffi::RenderOptions {
            breaks: false,
            linkify: false,
            typographer: false,
            math: true,
            front_matter: 0,
        };
        let doc = guarded(
            || -> Box<Document> { panic!("deliberate") },
            || Box::new(Document::unparsed(text, unparsed_blocks(text))),
        );
        assert_eq!(doc.blocks(), &blocks);
        // The next version is parsed from scratch, as if new.
        let next = reparse(&doc, text, &options);
        assert!(!next.spliced());
        assert_eq!(next.blocks(), &render::blocks(text, &options));
    }
}
