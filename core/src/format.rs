//! Document formatter: parse with GFM extensions and print back as CommonMark.

use comrak::options::ListStyleType;
use comrak::{Arena, Options, format_commonmark, parse_document};

fn options() -> Options<'static> {
    let mut options = Options::default();
    options.extension.table = true;
    options.extension.strikethrough = true;
    options.extension.autolink = true;
    options.extension.tasklist = true;
    options.extension.footnotes = true;
    options.extension.math_dollars = true;
    options.extension.front_matter_delimiter = Some("---".to_string());
    options.render.width = 0;
    // Indented code becomes fenced so every code block looks the same.
    options.render.prefer_fenced = true;
    options.render.list_style = ListStyleType::Dash;
    options
}

pub fn format(markdown: &str) -> String {
    let options = options();
    let arena = Arena::new();
    let root = parse_document(&arena, markdown, &options);
    let mut out = String::new();
    if format_commonmark(root, &options, &mut out).is_err() {
        return markdown.to_string();
    }
    align_tables(&out)
}

/// Terminal columns taken by a string; wide East Asian characters take two.
fn display_width(text: &str) -> usize {
    text.chars()
        .map(|c| match c as u32 {
            0x0300..=0x036F | 0x200B..=0x200D | 0xFE00..=0xFE0F => 0,
            0x1100..=0x115F
            | 0x2E80..=0xA4CF
            | 0xAC00..=0xD7A3
            | 0xF900..=0xFAFF
            | 0xFE30..=0xFE6F
            | 0xFF00..=0xFF60
            | 0xFFE0..=0xFFE6
            | 0x1F300..=0x1F64F
            | 0x1F900..=0x1F9FF
            | 0x20000..=0x3FFFD => 2,
            _ => 1,
        })
        .sum()
}

/// Splits `prefix| a | b |` into the prefix and trimmed cells. The prefix is
/// whatever list or quote markers precede the first pipe.
fn split_row(line: &str) -> Option<(&str, Vec<String>)> {
    let bar = line.find('|')?;
    let prefix = &line[..bar];
    if !prefix
        .chars()
        .all(|c| matches!(c, ' ' | '>' | '-' | '*' | '+' | '.' | ')') || c.is_ascii_digit())
    {
        return None;
    }
    let body = line[bar + 1..].trim_end();
    let body = body.strip_suffix('|').filter(|b| !b.ends_with('\\'))?;
    let mut cells = vec![String::new()];
    let mut escaped = false;
    for c in body.chars() {
        if escaped {
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else if c == '|' {
            cells.push(String::new());
            continue;
        }
        cells.last_mut().unwrap().push(c);
    }
    Some((prefix, cells.iter().map(|c| c.trim().to_string()).collect()))
}

/// Column alignment from a delimiter cell like `:---:`; None if it isn't one.
fn delimiter_alignment(cell: &str) -> Option<(bool, bool)> {
    let inner = cell.trim_start_matches(':').trim_end_matches(':');
    let valid = !inner.is_empty() && inner.chars().all(|c| c == '-');
    (valid && cell.len() - inner.len() <= 2).then(|| (cell.starts_with(':'), cell.ends_with(':')))
}

fn pad(text: &str, width: usize, left: bool, right: bool) -> String {
    let gap = width.saturating_sub(display_width(text));
    let (before, after) = match (left, right) {
        (true, true) => (gap / 2, gap - gap / 2),
        (false, true) => (gap, 0),
        _ => (0, gap),
    };
    format!("{}{}{}", " ".repeat(before), text, " ".repeat(after))
}

/// comrak prints tables with minimal padding; pad every column to its widest
/// cell. Works on the printed text, skipping fenced code.
fn align_tables(text: &str) -> String {
    let lines: Vec<&str> = text.split('\n').collect();
    let mut out: Vec<String> = Vec::with_capacity(lines.len());
    let mut fence: Option<(char, usize)> = None;
    let mut i = 0;
    while i < lines.len() {
        let line = lines[i];
        let trimmed = line.trim_start_matches([' ', '>']);
        if let Some((ch, len)) = fence {
            let run = trimmed.chars().take_while(|&c| c == ch).count();
            if run >= len && trimmed[run..].trim().is_empty() {
                fence = None;
            }
        } else if let Some(ch) = trimmed.chars().next().filter(|&c| c == '`' || c == '~') {
            let run = trimmed.chars().take_while(|&c| c == ch).count();
            if run >= 3 {
                fence = Some((ch, run));
            }
        }
        if fence.is_none()
            && let Some(end) = table_end(&lines, i)
        {
            out.extend(align_table(&lines[i..end]));
            i = end;
            continue;
        }
        out.push(line.to_string());
        i += 1;
    }
    out.join("\n")
}

/// End (exclusive) of the table starting at `start`, if one does.
fn table_end(lines: &[&str], start: usize) -> Option<usize> {
    let (_, header) = split_row(lines[start])?;
    let (prefix, delimiter) = split_row(lines.get(start + 1)?)?;
    if !prefix.chars().all(|c| matches!(c, ' ' | '>'))
        || delimiter.len() != header.len()
        || !delimiter.iter().all(|c| delimiter_alignment(c).is_some())
    {
        return None;
    }
    let mut end = start + 2;
    while end < lines.len()
        && split_row(lines[end]).is_some_and(|(p, _)| p.chars().all(|c| matches!(c, ' ' | '>')))
    {
        end += 1;
    }
    Some(end)
}

fn align_table(lines: &[&str]) -> Vec<String> {
    let rows: Vec<(&str, Vec<String>)> = lines.iter().filter_map(|l| split_row(l)).collect();
    let columns = rows[0].1.len();
    let alignment: Vec<(bool, bool)> = rows[1]
        .1
        .iter()
        .map(|c| delimiter_alignment(c).unwrap_or((false, false)))
        .collect();
    let mut widths = vec![3usize; columns];
    for (n, (_, cells)) in rows.iter().enumerate() {
        if n != 1 {
            for (w, cell) in widths.iter_mut().zip(cells) {
                *w = (*w).max(display_width(cell));
            }
        }
    }
    rows.iter()
        .enumerate()
        .map(|(n, (prefix, cells))| {
            let mut line = prefix.to_string();
            line.push('|');
            for col in 0..columns {
                let (left, right) = alignment[col];
                let cell = cells.get(col).map_or("", String::as_str);
                let body = if n == 1 {
                    let dashes = "-".repeat(widths[col] - usize::from(left) - usize::from(right));
                    format!(
                        "{}{dashes}{}",
                        if left { ":" } else { "" },
                        if right { ":" } else { "" }
                    )
                } else if left && right {
                    pad(cell, widths[col], true, true)
                } else {
                    pad(cell, widths[col], false, right)
                };
                line.push(' ');
                line.push_str(&body);
                line.push_str(" |");
            }
            line
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::format;

    #[test]
    fn already_formatted_text_is_unchanged() {
        let doc = "---\ntitle: x\n---\n\n# Title\n\nSome *emphasis* and **strong** text.\n\n- one\n- two\n  - nested\n\n1. first\n2. second\n\n> quote\n\n```rust\nfn main() {}\n```\n";
        let once = format(doc);
        assert_eq!(format(&once), once);
    }

    #[test]
    fn formatting_is_idempotent() {
        let doc = "Title\n=====\n\n* a\n* b\n\n    indented code\n\n| a | b |\n|--|--|\n| long cell | x |\n\n- [ ] todo\n- [x] done\n\n$$\nx^2\n$$\n\nText $a_b$ here.\n";
        let once = format(doc);
        assert_eq!(format(&once), once);
    }

    #[test]
    fn front_matter_is_preserved() {
        let doc = "---\ntitle:   Spaced\ntags: [a,  b]\n---\n\n# Heading\n";
        let out = format(doc);
        assert!(
            out.starts_with("---\ntitle:   Spaced\ntags: [a,  b]\n---\n"),
            "{out}"
        );
        assert!(out.contains("# Heading"));
    }

    #[test]
    fn math_is_preserved() {
        let doc = "Inline $a_b + c_d$ math.\n\n$$\n\\frac{a_1}{b_2}\n$$\n";
        let out = format(doc);
        assert!(out.contains("$a_b + c_d$"), "{out}");
        assert!(out.contains("$$\n\\frac{a_1}{b_2}\n$$"), "{out}");
    }

    #[test]
    fn tables_are_realigned() {
        let out = format("| a | b |\n|--|--|\n| long cell | x |\n");
        assert_eq!(
            out,
            "| a         | b   |\n| --------- | --- |\n| long cell | x   |\n"
        );
    }

    #[test]
    fn table_alignment_and_code_fences() {
        let out =
            format("| a | b |\n|:-|-:|\n| x | 12345 |\n\n```\n| not | table |\n|--|--|\n```\n");
        assert!(
            out.starts_with("| a   |     b |\n| :-- | ----: |\n| x   | 12345 |\n"),
            "{out}"
        );
        assert!(out.contains("| not | table |\n|--|--|\n"), "{out}");
    }

    #[test]
    fn task_boxes_are_preserved() {
        let out = format("* [ ] todo\n* [x] done\n");
        assert_eq!(out, "- [ ] todo\n- [x] done\n");
    }

    #[test]
    fn bullets_headings_and_fences_are_normalised() {
        let out = format("Title\n=====\n\n* a\n* b\n\nPara\n\n    code\n\n~~~\ntilde\n~~~\n");
        assert!(out.starts_with("# Title\n"), "{out}");
        assert!(out.contains("- a\n- b\n"), "{out}");
        assert!(out.contains("```\ncode\n```"), "{out}");
        assert!(!out.contains("~~~"), "{out}");
    }

    #[test]
    fn long_lines_are_not_wrapped() {
        let line = "word ".repeat(80);
        let out = format(&format!("{}\n", line.trim_end()));
        assert_eq!(out.trim_end().lines().count(), 1);
    }
}
