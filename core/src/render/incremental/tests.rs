//! Differential tests: after every edit, the incrementally maintained
//! document must be exactly a fresh parse of the same text.

use super::*;

/// xorshift64*: deterministic, no dependency.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n.max(1) as u64) as usize
    }

    fn chance(&mut self, percent: usize) -> bool {
        self.below(100) < percent
    }

    fn pick<'a, T>(&mut self, items: &'a [T]) -> &'a T {
        &items[self.below(items.len())]
    }
}

fn opts() -> RenderOptions {
    RenderOptions {
        breaks: false,
        linkify: true,
        typographer: false,
        math: true,
        front_matter: 2,
    }
}

/// Lines and line groups that open, close or continue every kind of block.
const SNIPPETS: &[&str] = &[
    "# Heading",
    "# Heading",
    "## Section two",
    "### Notes *em*",
    "Heading",
    "===",
    "---",
    "***",
    "___",
    "- item",
    "* item",
    "+ item",
    "1. one",
    "2) two",
    "10. ten",
    "- [ ] task",
    "- [x] done",
    "  - nested",
    "   - three spaces",
    "    indented code",
    "\tTabbed",
    "> quote",
    ">",
    "> > deeper",
    "  > lazy",
    "```",
    "```rust",
    "~~~",
    "````",
    "| a | b |",
    "|---|:-:|",
    "| 1 | 2 |",
    "<div>",
    "</div>",
    "<!-- comment",
    "-->",
    "<pre>",
    "</pre>",
    "<script>",
    "</script>",
    "<?php",
    "?>",
    "<![CDATA[",
    "]]>",
    "<!DOCTYPE html>",
    "<table><tr><td>",
    "</td></tr></table>",
    "<span>inline</span> text",
    "$$",
    "$x^2$ and $y$",
    "$$\na^2\n$$",
    "",
    "",
    "",
    "   ",
    "\t",
    "Text with *emph* and **strong** and `code`.",
    "A [link](http://x.y) and ![img](a.png) here.",
    "www.example.com and https://example.org",
    "Smart \"quotes\" -- dashes... 'single'",
    "trailing spaces  ",
    "\\# escaped",
    "&amp; &copy; entities",
    "1\\. not a list",
    "[x] [y][z] [^n]",
    "see [link][ref] and [^n]",
    "Title\n---",
    "Setext\n===",
    "a\n\nb",
    "- a\n\n  b",
    "- a\n- b\n- c",
    "1. a\n\n2. b",
    "```\ncode\n```",
    "```python\ndef f():\n\n    return 1\n```",
    "| h | i |\n|---|---|\n| r | s |",
    "<details>\n<summary>S</summary>\n\nBody\n\n</details>",
    "> q1\n>\n> q2",
    "- [ ] t1\n- [x] t2\n  - [ ] t3",
    "    code 1\n\n    code 2",
    "<div>\n*not md*\n</div>",
    "é中文 ünïcödé line",
    // Rare on purpose: these force full parses.
    "[ref]: http://x.y",
    "[^n]: footnote text",
];

const CHARS: &[char] = &[
    'a', 'b', 'x', ' ', ' ', '\n', '\n', '\n', '#', '-', '*', '+', '>', '`', '~', '|', '<', '/',
    '$', '!', '[', ']', '(', ')', '^', '=', '_', '1', '2', '.', '\t', '\\', '&', '"', '\'', 'é',
    '中', ':',
];

fn char_boundary(text: &str, mut i: usize) -> usize {
    while !text.is_char_boundary(i) {
        i -= 1;
    }
    i
}

fn line_starts(text: &str) -> Vec<usize> {
    let mut starts = vec![0];
    starts.extend(text.match_indices('\n').map(|(i, _)| i + 1));
    starts
}

/// A random position at the start of a line.
fn line_start(rng: &mut Rng, text: &str) -> usize {
    *rng.pick(&line_starts(text))
}

fn line_range(rng: &mut Rng, text: &str, max_lines: usize) -> (usize, usize) {
    let starts = line_starts(text);
    let first = rng.below(starts.len());
    let last = (first + 1 + rng.below(max_lines)).min(starts.len());
    let end = starts.get(last).copied().unwrap_or(text.len());
    (starts[first], end)
}

/// One random edit, mostly typing-like and local.
fn edit(rng: &mut Rng, text: &mut String) {
    let len = text.len();
    match rng.below(14) {
        0..=3 => {
            // Type a character (or a few).
            let at = char_boundary(text, rng.below(len + 1));
            let n = 1 + rng.below(3);
            let s: String = (0..n).map(|_| *rng.pick(CHARS)).collect();
            text.insert_str(at, &s);
        }
        4 | 5 => {
            // Delete a few characters.
            if len > 0 {
                let at = char_boundary(text, rng.below(len));
                let end = char_boundary(text, (at + 1 + rng.below(8)).min(len));
                let end = if end <= at {
                    (at + text[at..].chars().next().map_or(0, char::len_utf8)).min(len)
                } else {
                    end
                };
                text.replace_range(at..end, "");
            }
        }
        6 | 7 => {
            // Insert a snippet as new lines.
            let at = line_start(rng, text);
            let snippet = rng.pick(SNIPPETS);
            text.insert_str(at, &format!("{snippet}\n"));
        }
        8 => {
            // Replace a line with a snippet.
            let (start, end) = line_range(rng, text, 1);
            let snippet = rng.pick(SNIPPETS);
            let nl = if text[start..end].ends_with('\n') {
                "\n"
            } else {
                ""
            };
            text.replace_range(start..end, &format!("{snippet}{nl}"));
        }
        9 => {
            // Delete whole lines.
            let (start, end) = line_range(rng, text, 4);
            text.replace_range(start..end, "");
        }
        10 => {
            // Duplicate lines elsewhere (a paste).
            let (start, end) = line_range(rng, text, 6);
            let copy = text[start..end].to_string();
            let at = line_start(rng, text);
            text.insert_str(at, &copy);
        }
        11 => {
            // Join a line with the next.
            if let Some(i) = text
                .match_indices('\n')
                .map(|(i, _)| i)
                .nth(rng.below(text.matches('\n').count().max(1)))
            {
                text.replace_range(i..i + 1, if rng.chance(50) { "" } else { " " });
            }
        }
        12 => {
            // A blank line (or indentation) at a line start.
            let at = line_start(rng, text);
            text.insert_str(at, rng.pick(&["\n", "  ", "    ", "\t", "> ", "- "]));
        }
        _ => {
            // Type a word into the middle of a line.
            let at = char_boundary(text, rng.below(len + 1));
            text.insert_str(
                at,
                rng.pick(&["word ", " more", "Typing", "x", "**b**", "`c`", "[l](u)"]),
            );
        }
    }
}

#[derive(Default)]
struct Stats {
    checks: usize,
    spliced: usize,
}

fn describe(text: &str, line: usize) -> String {
    text.split('\n')
        .enumerate()
        .skip(line.saturating_sub(4))
        .take(9)
        .map(|(i, l)| format!("{i:6}: {l:?}\n"))
        .collect()
}

/// Reparses `text` from `prev` and checks it against a fresh parse.
fn check(
    prev: &Document,
    text: &str,
    options: &RenderOptions,
    stats: &mut Stats,
    context: &str,
) -> Document {
    let inc = reparse(prev, text, options);
    let full = Document::parse(text, options);
    stats.checks += 1;
    stats.spliced += inc.spliced as usize;
    if inc.blocks != full.blocks || inc.meta != full.meta {
        let i = (0..inc.blocks.len().max(full.blocks.len()))
            .find(|&i| {
                inc.blocks.get(i) != full.blocks.get(i) || inc.meta.get(i) != full.meta.get(i)
            })
            .unwrap();
        let line = full
            .blocks
            .get(i)
            .or(inc.blocks.get(i))
            .map_or(0, |b| b.first_line);
        let dir = std::env::temp_dir().join("vn-incremental-failure");
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::write(dir.join("before.md"), &prev.text);
        let _ = std::fs::write(dir.join("after.md"), text);
        panic!(
            "{context}: block {i} differs (spliced: {}; texts in {})\nincremental: {:#?} {:?}\nfull: {:#?} {:?}\nnew text near line {line}:\n{}",
            inc.spliced,
            dir.display(),
            inc.blocks.get(i),
            inc.meta.get(i),
            full.blocks.get(i),
            full.meta.get(i),
            describe(text, line)
        );
    }
    assert_eq!(inc.starts, full.starts, "{context}: line starts");
    assert_eq!(
        inc.front_matter_end, full.front_matter_end,
        "{context}: front matter"
    );
    assert_eq!(inc.local, full.local, "{context}: local");
    inc
}

/// A document like the performance test's: sections with headings,
/// paragraphs, lists, tasks, tables, code, quotes, math and HTML.
fn generated(sections: usize, seed: u64) -> String {
    let mut rng = Rng(seed);
    let mut doc = String::new();
    for i in 0..sections {
        doc.push_str(&format!("## Section {i}: notes on topic {}\n\n", i % 7));
        doc.push_str(&format!(
            "A paragraph with *emphasis*, **strong text**, a [link](https://example.com/page{i}) and `code`.\nIt keeps going for {i} more words.\n\n"
        ));
        match rng.below(8) {
            0 => doc.push_str("- first item\n- second item\n  - nested item\n- third item\n\n"),
            1 => doc.push_str("1. step one\n2. step two\n\n   continued\n3. step three\n\n"),
            2 => doc.push_str("> A quote with **bold**\n> and more.\n\n"),
            3 => doc.push_str(&format!(
                "| A | B |\n|---|---|\n| {i} | x |\n| y | {i} |\n\n"
            )),
            4 => doc.push_str(&format!("```python\ndef f{i}(x):\n\n    return x\n```\n\n")),
            5 => doc.push_str("- [ ] open task\n- [x] done task\n\n$$\nx^2\n$$\n\n"),
            6 => doc.push_str("<div align=\"center\">\n  <b>html</b>\n</div>\n\n"),
            _ => doc.push_str("Setext heading\n--------------\n\n    indented code\n\n"),
        }
    }
    doc
}

/// Hand-made documents full of blocks that reach across blank lines.
const NASTY: &[&str] = &[
    "- a\n\n- b\n\n  continued\n\n- c\n\nPara\n\n* other list\n\nText\n",
    "```\nopen fence\n\nText that is code\n\n# not a heading\n",
    "<!-- comment\n\nstill comment\n\n-->\n\nAfter\n\n<pre>\n\nraw\n\n</pre>\n\nEnd\n",
    "# A\n\n# A\n\n## A\n\nText\n\n# A\n\n- # A\n\n> # A\n",
    "Para\n===\n\nPara\n---\n\n| a |\n|---|\n| b |\nlazy row\n\nText\n",
    "    code\n\n    more code\n\nText\n\n  - item\n\n        code in item\n\nEnd\n",
    "> quote\nlazy\n\n> another\n\n>\n\nText\n",
    "- [ ] one\n- [x] two\n\n  - [ ] three\n\nText\n\n1. [x] four\n",
    "---\ntitle: Notes\ntags: x\n---\n\n# Body\n\nText\n\n---\n\nMore\n",
    "---\nnot closed\n\n# Heading\n\nText\n",
    "<div>\n\n*md inside*\n\n</div>\n\n<script>\nlet a = 1;\n\nlet b = 2;\n</script>\n\nEnd\n",
    "$$\nx\n$$\n\nText $y$\n\n- $$\n  z\n  $$\n",
    "1. a\n2. b\n\n3) c\n\n10. d\n\nText\n",
    "Text\n\n~~~\n```\n~~~\n\n````\n```\n````\n\nEnd\n",
    "\tTab\n\n\t- tab list\n\nText\n\n<table><tr><td>\n\n| x |\n|---|\n\n</td></tr></table>\n",
    "é中文\n\n# Ünïcödé\n\n中文段落\n\n- 項目\n",
    "text\n| a | b |\n|---|---|\n| 1 | 2 |\n\nPara\n# Heading right after\ntext\n- list right after\n\n> q\n| x |\n|---|\n",
    "---\ntext\n\n# H\n\nmore\n\n# Again\n\n---\n\nlast\n",
];

fn run(
    name: &str,
    start: &str,
    options: &RenderOptions,
    edits: usize,
    seed: u64,
    total: &mut Stats,
) {
    // VN_DIFF_SEED varies the runs (VN_DIFF_EDITS makes them longer).
    let extra: u64 = std::env::var("VN_DIFF_SEED")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    let mut rng = Rng(seed ^ extra.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let mut text = start.to_string();
    let mut doc = Document::parse(&text, options);
    let mut stats = Stats::default();
    let mut history = vec![text.clone()];
    let mut step = 0;
    while step < edits {
        // Several edits before one parse, as when typing goes on while a
        // background parse runs; now and then a jump to an older version.
        if rng.chance(2) && history.len() > 1 {
            text = rng.pick(&history).clone();
        } else if !is_local(&text) && rng.chance(20) {
            // Definitions and `\r` make every parse a full one; take them
            // out again so that most of the run tests splicing.
            text = text.replace("]:", "] :").replace('\r', "");
        } else {
            let n = if rng.chance(25) { 2 + rng.below(4) } else { 1 };
            for _ in 0..n {
                edit(&mut rng, &mut text);
                step += 1;
            }
        }
        // Keep documents from growing or shrinking without bound.
        if text.len() > 3 * start.len() + 4096 || text.len() < start.len() / 3 {
            text = start.to_string();
        }
        doc = check(
            &doc,
            &text,
            options,
            &mut stats,
            &format!("{name} step {step}"),
        );
        if rng.chance(5) {
            history.push(text.clone());
        }
    }
    println!("{name}: {} checks, {} spliced", stats.checks, stats.spliced);
    total.checks += stats.checks;
    total.spliced += stats.spliced;
}

fn edits_per_document() -> usize {
    std::env::var("VN_DIFF_EDITS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(600)
}

#[test]
fn random_edits_match_full_parse() {
    let edits = edits_per_document();
    let mut total = Stats::default();
    let mut corpus: Vec<(String, String)> = vec![
        (
            "README.md".into(),
            include_str!("../../../../README.md").into(),
        ),
        (
            "design.md".into(),
            include_str!("../../../../docs/design.md").into(),
        ),
        ("generated".into(), generated(120, 3)),
        ("generated-small".into(), generated(12, 5)),
    ];
    for (i, nasty) in NASTY.iter().enumerate() {
        // Each nasty case inside ordinary text, so that windows have room.
        corpus.push((
            format!("nasty {i}"),
            format!(
                "{}{}{}",
                generated(4, i as u64 + 11),
                nasty,
                generated(4, i as u64 + 29)
            ),
        ));
        corpus.push((format!("nasty {i} alone"), nasty.to_string()));
    }
    for (i, (name, text)) in corpus.iter().enumerate() {
        let scale = if text.len() > 2000 { 1 } else { 2 };
        run(
            name,
            text,
            &opts(),
            edits / scale + 50,
            0x9E37_79B9_7F4A_7C15 ^ (i as u64 + 1),
            &mut total,
        );
    }
    println!("total: {} checks, {} spliced", total.checks, total.spliced);
    // The fast path must stay in use: most edits are local.
    assert!(
        total.spliced * 3 > total.checks,
        "{} of {}",
        total.spliced,
        total.checks
    );
}

#[test]
fn random_edits_match_full_parse_with_other_options() {
    let edits = edits_per_document() / 3;
    let mut total = Stats::default();
    let variants = [
        RenderOptions {
            breaks: true,
            linkify: false,
            typographer: true,
            math: false,
            front_matter: 0,
        },
        RenderOptions {
            breaks: false,
            linkify: true,
            typographer: true,
            math: true,
            front_matter: 1,
        },
    ];
    for (v, options) in variants.iter().enumerate() {
        for (i, nasty) in NASTY.iter().enumerate() {
            let text = format!(
                "{}{}{}",
                generated(3, i as u64 + 7),
                nasty,
                generated(3, i as u64 + 3)
            );
            run(
                &format!("options {v} nasty {i}"),
                &text,
                options,
                edits + 30,
                77 + (v * 100 + i) as u64,
                &mut total,
            );
        }
    }
    println!("total: {} checks, {} spliced", total.checks, total.spliced);
    assert!(
        total.spliced * 3 > total.checks,
        "{} of {}",
        total.spliced,
        total.checks
    );
}

#[test]
fn typing_in_a_large_document_splices() {
    let text = generated(400, 1);
    let doc = Document::parse(&text, &opts());
    let at = text.len() / 2;
    let at = at + text[at..].find("It keeps going").unwrap();
    let mut edited = text.clone();
    edited.insert_str(at, "Typed ");
    let mut stats = Stats::default();
    let next = check(&doc, &edited, &opts(), &mut stats, "typing");
    assert!(next.spliced());
    // A new line with a task list below the edit moves every later task.
    let mut edited2 = edited.clone();
    edited2.insert_str(at, "\n\n- [ ] new task\n\n");
    let next2 = check(&next, &edited2, &opts(), &mut stats, "new lines");
    assert!(next2.spliced());
}

#[test]
fn global_constructs_parse_everything() {
    let text = generated(50, 2);
    let doc = Document::parse(&text, &opts());
    let mut stats = Stats::default();
    for insert in ["[ref]: http://x.y\n\n", "[^n]: note\n\n", "a\r\nb\n\n"] {
        let mut edited = text.clone();
        let at = text.len() / 2;
        let at = at + text[at..].find("\n\n").unwrap() + 2;
        edited.insert_str(at, insert);
        assert!(
            !check(&doc, &edited, &opts(), &mut stats, insert).spliced(),
            "{insert:?}"
        );
    }
    // Different options: everything.
    let mut other = opts();
    other.typographer = true;
    assert!(!reparse(&doc, &text, &other).spliced());
    // An unclosed fence swallows the rest of the document: the window's end
    // check sees it.
    let mut edited = text.clone();
    let at = text.find("A paragraph").unwrap();
    edited.insert_str(at, "```\n");
    let next = check(&doc, &edited, &opts(), &mut stats, "fence");
    assert!(!next.spliced());
}

#[test]
fn front_matter_made_or_broken_far_away() {
    let mut stats = Stats::default();
    // A thematic break at the start becomes front matter when a later line
    // turns into its closing delimiter.
    let base = "---\ntext\n\n# H\n\nmore\n\n# End\n\nlast\n";
    let doc = Document::parse(base, &opts());
    check(
        &doc,
        &base.replace("more", "---"),
        &opts(),
        &mut stats,
        "made",
    );
    // Real front matter: typing after it splices, breaking it does not.
    let fm = "---\ntitle: x\n---\n\n# H\n\nmore\n\n---\n\nend\n";
    let doc = Document::parse(fm, &opts());
    assert!(
        check(
            &doc,
            &fm.replace("more", "more text"),
            &opts(),
            &mut stats,
            "after"
        )
        .spliced()
    );
    check(
        &doc,
        &fm.replace("title: x\n---", "title: x\n--"),
        &opts(),
        &mut stats,
        "broken",
    );
    check(
        &doc,
        &fm.replace("---\n\n# H", "---\n# H"),
        &opts(),
        &mut stats,
        "blank line",
    );
}

#[test]
fn tables_that_start_inside_a_paragraph() {
    // The table's header row was paragraph text first; breaking its delimiter
    // row joins it to the paragraph line above.
    let base = "Intro\n\ntext\n| a | b |\n|---|---|\n| 1 | 2 |\n\nAfter\n";
    let doc = Document::parse(base, &opts());
    let mut stats = Stats::default();
    check(
        &doc,
        &base.replace("|---|---|", "|-x-|---|"),
        &opts(),
        &mut stats,
        "delimiter",
    );
    check(
        &doc,
        &base.replace("| 1 | 2 |", "| 1 | 2 | 3 |"),
        &opts(),
        &mut stats,
        "row",
    );
}

#[test]
fn heading_anchors_count_across_the_window() {
    let base = "# Same\n\nText\n\n# Other\n\nMore\n\n# Same\n\nEnd\n";
    let doc = Document::parse(base, &opts());
    let mut stats = Stats::default();
    // Renaming the middle heading to the shared slug renumbers the last.
    let renamed = base.replace("# Other", "# Same");
    check(&doc, &renamed, &opts(), &mut stats, "rename");
    let typed = base.replace("More", "More text");
    assert!(check(&doc, &typed, &opts(), &mut stats, "typing").spliced());
}

#[test]
fn edges_of_the_text() {
    // To and from empty text, blank text, and text without a final newline.
    let versions = [
        "",
        "\n",
        "a",
        "# T\n\npara",
        "# T\n\npara\n",
        "# T\n\npara\n\n",
        "   \n\n\t\n",
        "# T\n\npara\n\n```",
        "# T\n\npara\n\n- x",
        "",
        "x\n\ny\n\nz",
    ];
    let mut stats = Stats::default();
    for (i, from) in versions.iter().enumerate() {
        let doc = Document::parse(from, &opts());
        for to in &versions {
            check(
                &doc,
                to,
                &opts(),
                &mut stats,
                &format!("{i} {from:?} -> {to:?}"),
            );
        }
    }
}

/// Parses `text` in windows of `window` bytes and checks it against a whole
/// parse; returns whether it was parsed in windows at all.
fn check_windowed(text: &str, options: &RenderOptions, window: usize, context: &str) -> bool {
    let Some(windowed) = parse_windowed(text, options, window) else {
        return false;
    };
    let full = Document::parse(text, options);
    if windowed.blocks != full.blocks || windowed.meta != full.meta {
        let i = (0..windowed.blocks.len().max(full.blocks.len()))
            .find(|&i| {
                windowed.blocks.get(i) != full.blocks.get(i)
                    || windowed.meta.get(i) != full.meta.get(i)
            })
            .unwrap();
        let line = full
            .blocks
            .get(i)
            .or(windowed.blocks.get(i))
            .map_or(0, |b| b.first_line);
        let dir = std::env::temp_dir().join("vn-windowed-failure");
        let _ = std::fs::create_dir_all(&dir);
        let _ = std::fs::write(dir.join("text.md"), text);
        panic!(
            "{context}: block {i} differs in windows of {window} (text in {})\nwindowed: {:#?} {:?}\nwhole: {:#?} {:?}\nnear line {line}:\n{}",
            dir.display(),
            windowed.blocks.get(i),
            windowed.meta.get(i),
            full.blocks.get(i),
            full.meta.get(i),
            describe(text, line)
        );
    }
    assert_eq!(windowed.starts, full.starts, "{context}: line starts");
    assert_eq!(
        windowed.front_matter_end, full.front_matter_end,
        "{context}: front matter"
    );
    assert_eq!(windowed.local, full.local, "{context}: local");
    assert!(!windowed.spliced);
    true
}

/// Parsing in windows gives exactly the whole parse's blocks, for windows of
/// every size down to a line (a boundary tried at every line that may be
/// one), on the corpus and on the texts of random edits to it.
#[test]
fn windowed_parse_matches_whole_parse() {
    let mut corpus: Vec<(String, String)> = vec![
        (
            "README.md".into(),
            include_str!("../../../../README.md").into(),
        ),
        (
            "design.md".into(),
            include_str!("../../../../docs/design.md").into(),
        ),
        ("generated".into(), generated(120, 3)),
    ];
    for (i, nasty) in NASTY.iter().enumerate() {
        corpus.push((
            format!("nasty {i}"),
            format!(
                "{}{}{}",
                generated(4, i as u64 + 11),
                nasty,
                generated(4, i as u64 + 29)
            ),
        ));
        corpus.push((format!("nasty {i} alone"), nasty.to_string()));
    }
    let edits = edits_per_document() / 4;
    let mut windowed = 0;
    let mut checks = 0;
    for (i, (name, start)) in corpus.iter().enumerate() {
        let mut rng = Rng(0x5851_F42D_4C95_7F2D ^ (i as u64 + 1));
        let mut text = start.clone();
        for step in 0..edits {
            for window in [1, 80, 700, 5000] {
                checks += 1;
                windowed +=
                    check_windowed(&text, &opts(), window, &format!("{name} step {step}")) as usize;
            }
            if !is_local(&text) && rng.chance(20) {
                // As in run(): definitions and `\r` make every parse a whole one.
                text = text.replace("]:", "] :").replace('\r', "");
            } else {
                edit(&mut rng, &mut text);
            }
            if text.len() > 3 * start.len() + 4096 || text.len() < start.len() / 3 {
                text = start.clone();
            }
        }
    }
    println!("windowed parses: {windowed} of {checks}");
    // Most texts have no definitions or footnotes.
    assert!(windowed * 2 > checks, "{windowed} of {checks}");
}

/// The same with the other options, which change what blocks are.
#[test]
fn windowed_parse_matches_whole_parse_with_other_options() {
    let variants = [
        RenderOptions {
            breaks: true,
            linkify: false,
            typographer: true,
            math: false,
            front_matter: 0,
        },
        RenderOptions {
            breaks: false,
            linkify: true,
            typographer: false,
            math: true,
            front_matter: 1,
        },
    ];
    for (v, options) in variants.iter().enumerate() {
        for (i, nasty) in NASTY.iter().enumerate() {
            let text = format!(
                "{}{}{}",
                generated(6, i as u64 + 41),
                nasty,
                generated(6, i as u64 + 43)
            );
            for window in [1, 120, 900] {
                check_windowed(&text, options, window, &format!("variant {v} nasty {i}"));
            }
        }
    }
}

/// Large texts go through windows of the real size and give the same.
#[test]
fn large_texts_parse_in_windows() {
    let text = generated(14_000, 21);
    assert!(text.len() > 2 * PARSE_WINDOW);
    assert!(check_windowed(&text, &opts(), PARSE_WINDOW, "generated"));
    let large = Document::parse_large(&text, &opts());
    let full = Document::parse(&text, &opts());
    assert!(large.blocks == full.blocks && large.meta == full.meta);
    // A fence that runs over many windows' worth of boundaries.
    let mut fenced = generated(200, 22);
    fenced.push_str("```\n");
    for i in 0..60_000 {
        fenced.push_str(&format!("line {i}\n\nword {i}\n"));
    }
    fenced.push_str("```\n\n");
    fenced.push_str(&generated(200, 23));
    assert!(check_windowed(&fenced, &opts(), PARSE_WINDOW, "fenced"));
    assert!(check_windowed(&fenced, &opts(), 4096, "fenced, small windows"));
}

/// `cargo test --release perf_reparse -- --ignored --nocapture`
#[test]
#[ignore]
fn perf_reparse_large_documents() {
    for sections in [5_000, 50_000] {
        let text = generated(sections, 9);
        let start = std::time::Instant::now();
        let mut current = Document::parse(&text, &opts());
        let full = start.elapsed();
        let at = text.len() / 2;
        let at = at + text[at..].find("It keeps going").unwrap();
        let mut times = Vec::new();
        let mut edited = text.clone();
        for k in 0..7 {
            edited.insert_str(at, "a");
            let start = std::time::Instant::now();
            let next = reparse(&current, &edited, &opts());
            times.push(start.elapsed());
            assert!(next.spliced(), "{k}");
            current = next;
        }
        times.sort();
        println!(
            "{} bytes, {} blocks: full parse {full:?}, reparse after a keystroke {:?} (median of 7)",
            text.len(),
            current.blocks.len(),
            times[3]
        );
    }
}
