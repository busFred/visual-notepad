//! Mermaid diagrams: mermaid-rs-renderer lays the diagram out as SVG and
//! resvg rasterizes it to PNG for Qt.

use std::collections::HashMap;
use std::sync::{Arc, OnceLock};

use mermaid_rs_renderer::{DiagramKind, Graph, LayoutConfig, Theme, compute_layout, parse_mermaid_strict, render_svg};
use resvg::tiny_skia::{Pixmap, Transform};
use resvg::usvg::{self, fontdb};

/// System fonts are slow to scan, so load them once.
fn fonts() -> Arc<fontdb::Database> {
    static FONTS: OnceLock<Arc<fontdb::Database>> = OnceLock::new();
    FONTS
        .get_or_init(|| {
            let mut db = fontdb::Database::new();
            db.load_system_fonts();
            Arc::new(db)
        })
        .clone()
}

/// "auto" follows the app's light theme with the modern look.
fn theme(name: &str) -> Theme {
    let mut theme = match name {
        "auto" | "" => Theme::modern(),
        other => Theme::from_name(other).unwrap_or_else(Theme::modern),
    };
    if name == "auto" || name.is_empty() {
        theme.background = String::from("transparent");
    }
    theme
}

/// Longest diagram source (bytes) that is laid out. Real diagrams are a few
/// KB; layout time and memory grow with the source, so a pasted blob must not
/// keep a worker busy for minutes.
const MAX_SOURCE_BYTES: usize = 100 * 1024;
/// Largest SVG (bytes) that is parsed for rasterizing.
const MAX_SVG_BYTES: usize = 16 * 1024 * 1024;
/// Most pixels a diagram may have at the scale it is drawn: 64 MiB of RGBA,
/// which is what the GUI's diagram cache can hold.
const MAX_PIXELS: u64 = 16 * 1024 * 1024;
/// Longest side (pixels) of a diagram; far beyond what a column can show.
const MAX_SIDE: f64 = 32768.0;
/// Scale below which a too-large diagram is given up instead of shrunk.
const MIN_SCALE: f32 = 0.5;

/// The largest scale at or below `wanted` at which a `width` x `height`
/// diagram stays within `MAX_PIXELS`, or `None` if even `MIN_SCALE` is too
/// much. Ordinary diagrams get `wanted` back unchanged.
fn fit_scale(width: u32, height: u32, wanted: f32) -> Option<f32> {
    let pixels_at = |scale: f32| {
        let w = (width as f64 * scale as f64).ceil();
        let h = (height as f64 * scale as f64).ceil();
        (w.max(1.0), h.max(1.0))
    };
    let fits = |scale: f32| {
        let (w, h) = pixels_at(scale);
        w * h <= MAX_PIXELS as f64 && w <= MAX_SIDE && h <= MAX_SIDE
    };
    if fits(wanted) {
        return Some(wanted);
    }
    let (w1, h1) = (width.max(1) as f64, height.max(1) as f64);
    // Slightly under the exact limits so that rounding up the sides still fits.
    let by_area = (MAX_PIXELS as f64 / (w1 * h1)).sqrt();
    let by_side = (MAX_SIDE / w1).min(MAX_SIDE / h1);
    let mut scale = (by_area.min(by_side) * 0.999) as f32;
    while scale >= MIN_SCALE {
        if fits(scale) {
            return Some(scale);
        }
        scale *= 0.995;
    }
    None
}

/// Longest layout (milliseconds) a diagram may be expected to need.
///
/// mermaid-rs-renderer cannot be told to stop once a layout has begun, and
/// its edge routing grows with the square of the number of edges: measured
/// here (release build, one thread), flowcharts of 40, 80 and 120 edges took
/// 0.1 to 0.8, 0.9 to 1.4 and 2 to 2.6 s depending on their shape, 400 edges
/// of diamonds 23 s, and 1200 edges two minutes. A hard deadline is not
/// possible (an abandoned thread would go on burning CPU), so the size of
/// the diagram is limited before the layout starts.
const MAX_LAYOUT_MS: f64 = 1500.0;
/// Most non-blank source lines of a diagram kind without a cost model below
/// (journey and C4 take about 1 s at 2500 lines).
const MAX_OTHER_LINES: usize = 3000;

/// Rough worst-case layout time in milliseconds from what the parser found,
/// or `None` for a kind that scales linearly (measured up to 100 KB of
/// source in well under a second). The models are quadratic in the number of
/// edges, with coefficients taken from the slowest shapes tried: complete and
/// bipartite graphs, diamond chains, rings with back edges, trees, fans.
fn estimated_layout_ms(graph: &Graph) -> Option<f64> {
    let edges = graph.edges.len() as f64;
    let quadratic = |coefficient: f64, size: f64| Some(coefficient * size * size);
    match graph.kind {
        DiagramKind::Flowchart => {
            // Edges that join others (several arrive at one node) must be
            // routed around them and cost the most; edges that fork cost
            // about 0.7 of that (a fan of 120 edges takes 2 s, a tree of
            // 300 edges 3.7 s); an edge on a plain path 1/8 (a chain of 300
            // boxes takes 0.3 s, 1000 of them 3.7 s).
            let mut out_degree: HashMap<&str, u32> = HashMap::new();
            let mut in_degree: HashMap<&str, u32> = HashMap::new();
            for edge in &graph.edges {
                *out_degree.entry(edge.from.as_str()).or_default() += 1;
                *in_degree.entry(edge.to.as_str()).or_default() += 1;
            }
            let routed: f64 = graph
                .edges
                .iter()
                .map(|edge| {
                    if in_degree[edge.to.as_str()] > 1 {
                        1.0
                    } else if out_degree[edge.from.as_str()] > 1 {
                        0.7
                    } else {
                        0.125
                    }
                })
                .sum();
            // The linear part is what small diagrams cost per edge.
            Some(10.0 * routed + 0.1 * routed * routed)
        }
        DiagramKind::Class => quadratic(0.04, edges),
        DiagramKind::State => quadratic(0.08, edges),
        DiagramKind::Er => quadratic(0.025, edges),
        DiagramKind::Requirement => quadratic(0.04, edges),
        DiagramKind::Packet => quadratic(0.0035, edges),
        DiagramKind::Sequence | DiagramKind::ZenUML => {
            // Placing the labels compares each with the others.
            let items = edges
                + (graph.sequence_notes.len() + graph.sequence_activations.len()) as f64
                + 2.0 * graph.sequence_frames.len() as f64;
            // Every participant adds about 0.4 ms (1500 take 0.55 s).
            Some(0.0016 * items * items + 0.4 * graph.sequence_participants.len() as f64)
        }
        _ => None,
    }
}

/// True if laying the diagram out is expected to stay within `MAX_LAYOUT_MS`.
fn layout_is_affordable(code: &str, graph: &Graph) -> bool {
    match estimated_layout_ms(graph) {
        Some(ms) => ms <= MAX_LAYOUT_MS,
        None => code.lines().filter(|line| !line.trim().is_empty()).count() <= MAX_OTHER_LINES,
    }
}

/// Renders a diagram to premultiplied RGBA8 pixels at the given scale (device
/// pixel ratio); a diagram too big for that scale is drawn at a smaller one.
/// Returns the width, height, pixels and the scale actually used.
pub fn raster(code: &str, theme_name: &str, scale: f32) -> Result<(u32, u32, Vec<u8>, f32), String> {
    if code.len() > MAX_SOURCE_BYTES {
        return Err(String::from("diagram too large"));
    }
    let theme = theme(theme_name);
    // The same steps as `render_with_options`, with a look at the diagram
    // between parsing and the layout, which cannot be interrupted.
    let parsed = parse_mermaid_strict(code).map_err(|e| e.to_string())?;
    if !layout_is_affordable(code, &parsed.graph) {
        return Err(String::from("diagram too large"));
    }
    let layout_config = LayoutConfig::default();
    let layout = compute_layout(&parsed.graph, &theme, &layout_config);
    drop(parsed);
    let svg = render_svg(&layout, &theme, &layout_config);
    drop(layout);
    if svg.len() > MAX_SVG_BYTES {
        return Err(String::from("diagram too large"));
    }

    let options = usvg::Options { fontdb: fonts(), font_family: theme.font_family.clone(), ..Default::default() };
    let tree = usvg::Tree::from_str(&svg, &options).map_err(|e| e.to_string())?;
    drop(svg);
    let wanted = if scale.is_finite() { scale.clamp(MIN_SCALE, 4.0) } else { 1.0 };
    let size = tree.size().to_int_size();
    // Decide on the size before allocating anything.
    let scale = fit_scale(size.width(), size.height(), wanted).ok_or("diagram too large")?;
    let width = ((size.width() as f32) * scale).ceil() as u32;
    let height = ((size.height() as f32) * scale).ceil() as u32;
    let mut pixmap = Pixmap::new(width.max(1), height.max(1)).ok_or("diagram too large")?;
    resvg::render(&tree, Transform::from_scale(scale, scale), &mut pixmap.as_mut());
    Ok((pixmap.width(), pixmap.height(), pixmap.take(), scale))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_flowchart() {
        let (w, h, pixels, _) = raster("flowchart LR; A-->B", "auto", 1.0).expect("render");
        assert!(w > 0 && h > 0);
        assert_eq!(pixels.len(), (w * h * 4) as usize);
        assert!(pixels.iter().any(|&b| b != 0));
    }

    #[test]
    fn reports_syntax_errors() {
        assert!(raster("not a diagram %%%", "default", 1.0).is_err());
    }

    /// A chain of `n` boxes, each with a `words`-word label.
    fn chain(n: usize, words: usize) -> String {
        let label = "label ".repeat(words);
        let mut code = String::from("flowchart LR\n");
        for i in 0..n {
            code.push_str(&format!("N{i}[\"{label}\"] --> N{}\n", i + 1));
        }
        code
    }

    #[test]
    fn small_diagram_keeps_requested_scale() {
        let (w, h, pixels, scale) = raster("flowchart LR; A-->B", "auto", 3.0).expect("render");
        assert_eq!(scale, 3.0);
        assert_eq!(pixels.len(), (w * h * 4) as usize);
        let (w1, h1, _, _) = raster("flowchart LR; A-->B", "auto", 1.0).expect("render");
        assert!(w >= w1 * 3 - 3 && h >= h1 * 3 - 3);
    }

    #[test]
    fn oversized_source_is_rejected_quickly() {
        let start = std::time::Instant::now();
        let label = format!("flowchart LR\nA[\"{}\"] --> B\n", "x".repeat(200_000));
        assert_eq!(raster(&label, "auto", 1.0).err().as_deref(), Some("diagram too large"));
        let blob = format!("flowchart LR\n{}", "A-->B\n".repeat(50_000));
        assert_eq!(raster(&blob, "auto", 1.0).err().as_deref(), Some("diagram too large"));
        assert!(start.elapsed().as_secs() < 2, "took {:?}", start.elapsed());
    }

    #[test]
    fn large_legitimate_diagram_still_renders() {
        // About 20 KB of source: bigger than anyone draws by hand.
        let code = chain(120, 3);
        assert!(code.len() < MAX_SOURCE_BYTES);
        let (w, h, pixels, scale) = raster(&code, "auto", 4.0).expect("render");
        assert!((MIN_SCALE..=4.0).contains(&scale));
        assert!(u64::from(w) * u64::from(h) <= MAX_PIXELS);
        assert_eq!(pixels.len(), (w * h * 4) as usize);
        assert!(pixels.iter().any(|&b| b != 0));
    }

    #[test]
    fn large_diagram_is_drawn_at_a_smaller_scale() {
        // Too big at scale 4 but fine at a lower one: shrinks instead of failing.
        let code = chain(60, 20);
        let (w, h, _, scale) = raster(&code, "auto", 4.0).expect("render");
        assert!(scale < 4.0 && scale >= MIN_SCALE, "scale {scale}");
        assert!(u64::from(w) * u64::from(h) <= MAX_PIXELS);
    }

    #[test]
    fn huge_diagrams_fail_without_allocating_gigabytes() {
        let start = std::time::Instant::now();
        // Within the source limit, but far too many pixels at any scale.
        let code = chain(700, 20);
        assert!(code.len() <= MAX_SOURCE_BYTES, "{}", code.len());
        assert_eq!(raster(&code, "auto", 4.0).err().as_deref(), Some("diagram too large"));
        assert!(start.elapsed().as_secs() < 20, "took {:?}", start.elapsed());
    }

    #[test]
    fn fit_scale_limits_pixels() {
        assert_eq!(fit_scale(800, 600, 4.0), Some(4.0));
        assert_eq!(fit_scale(0, 0, 4.0), Some(4.0));
        let s = fit_scale(4000, 4000, 4.0).expect("fits when shrunk");
        assert!((1.0..4.0).contains(&s));
        let side = (4000.0 * s).ceil() as u64;
        assert!(side * side <= MAX_PIXELS);
        assert_eq!(fit_scale(100_000, 10, 1.0), None); // longer than MAX_SIDE even at 0.5
        assert_eq!(fit_scale(100_000, 1000, 1.0), None);
        assert!(fit_scale(40_000, 10, 4.0).is_some());
        assert_eq!(fit_scale(u32::MAX, u32::MAX, 1.0), None);
        assert_eq!(fit_scale(1000, 100_000, 0.5), None);
    }

    #[test]
    fn non_finite_scale_is_harmless() {
        assert!(raster("flowchart LR; A-->B", "auto", f32::NAN).is_ok());
        assert!(raster("flowchart LR; A-->B", "auto", f32::INFINITY).is_ok());
    }

    // --- Layout cost limits ---

    #[test]
    fn the_steps_are_those_of_render_with_options() {
        let code = "flowchart LR\n A[Start] --> B{Is it?}\n B -->|Yes| C[OK]\n B -->|No| D[End]\n";
        let options = mermaid_rs_renderer::RenderOptions { theme: theme("auto"), ..Default::default() };
        let expected = mermaid_rs_renderer::render_with_options(code, options.clone()).expect("render");
        let parsed = parse_mermaid_strict(code).expect("parse");
        let layout = compute_layout(&parsed.graph, &options.theme, &LayoutConfig::default());
        assert_eq!(render_svg(&layout, &options.theme, &LayoutConfig::default()), expected);
    }

    /// Diagrams of one kind and shape with about `n` edges. The shapes are the
    /// slowest ones found when measuring (see `estimated_layout_ms`).
    type Shape = (&'static str, fn(usize) -> String);

    fn diamonds(n: usize) -> String {
        let mut code = String::from("flowchart LR\n");
        for i in 0..n / 4 {
            code.push_str(&format!("D{i}[a] --> B{i}[b]\nD{i} --> C{i}[c]\nB{i} --> D{}\nC{i} --> D{}\n", i + 1, i + 1));
        }
        code
    }

    fn bipartite(n: usize) -> String {
        let side = ((n as f64).sqrt() as usize).max(1);
        let mut code = String::from("flowchart TD\n");
        for i in 0..side {
            for j in 0..side {
                code.push_str(&format!("A{i} --> B{j}\n"));
            }
        }
        code
    }

    fn complete(n: usize) -> String {
        let mut size = 3;
        while size * (size - 1) / 2 < n {
            size += 1;
        }
        let mut code = String::from("flowchart TD\n");
        for (count, (i, j)) in (0..size).flat_map(|i| (i + 1..size).map(move |j| (i, j))).enumerate() {
            if count >= n {
                break;
            }
            code.push_str(&format!("K{i} --> K{j}\n"));
        }
        code
    }

    fn fan(n: usize) -> String {
        (1..=n).fold(String::from("flowchart TD\n"), |code, i| code + &format!("R --> N{i}\n"))
    }

    fn tree(n: usize) -> String {
        (1..=n).fold(String::from("flowchart TD\n"), |code, i| code + &format!("N{} --> N{i}\n", (i - 1) / 3))
    }

    fn ring(n: usize) -> String {
        let nodes = (n / 3).max(4);
        let mut code = String::from("flowchart LR\n");
        for i in 0..nodes {
            code.push_str(&format!(
                "N{i}[Node {i}] -->|next| N{}\nN{i} -.->|skip| N{}\nN{} ==>|back| N{i}\n",
                (i + 1) % nodes,
                (i + 3) % nodes,
                (i + 2) % nodes
            ));
        }
        code
    }

    fn pipeline(n: usize) -> String {
        (0..n).fold(String::from("flowchart TD\n"), |code, i| code + &format!("N{i}[Step {i}] --> N{}\n", i + 1))
    }

    fn dense(kind: &'static str, arrow: &'static str, n: usize) -> String {
        let nodes = (n / 2).max(4);
        let mut code = format!("{kind}\n");
        for i in 0..nodes {
            code.push_str(&format!("N{i} {arrow} N{}\nN{i} {arrow} N{}\n", (i * 7 + 1) % nodes, (i * 13 + 5) % nodes));
        }
        code
    }

    fn class_dense(n: usize) -> String {
        dense("classDiagram", "-->", n)
    }

    fn state_dense(n: usize) -> String {
        dense("stateDiagram-v2", "-->", n)
    }

    fn er_dense(n: usize) -> String {
        let nodes = (n / 2).max(4);
        let mut code = String::from("erDiagram\n");
        for i in 0..nodes {
            code.push_str(&format!(
                "E{i} ||--o{{ E{} : a\nE{i} ||--o{{ E{} : b\n",
                (i * 7 + 1) % nodes,
                (i * 13 + 5) % nodes
            ));
        }
        code
    }

    fn requirements(n: usize) -> String {
        let mut code = String::from("requirementDiagram\n");
        for i in 0..=n {
            code.push_str(&format!("requirement r{i} {{\n id: {i}\n text: t{i}\n risk: high\n}}\n"));
        }
        for i in 1..=n {
            code.push_str(&format!("r{} - satisfies -> r{i}\n", (i * 7) % i));
        }
        code
    }

    fn packets(n: usize) -> String {
        (0..n).fold(String::from("packet\n"), |code, i| code + &format!("  {}-{}: \"F{i}\"\n", i * 4, i * 4 + 3))
    }

    fn messages(n: usize) -> String {
        let mut code = String::from("sequenceDiagram\n");
        for i in 0..10 {
            code.push_str(&format!("participant P{i}\n"));
        }
        for i in 0..n {
            code.push_str(&format!("P{} ->> P{}: message {i}\n", i % 10, (i * 3 + 1) % 10));
        }
        code
    }

    fn framed_messages(n: usize) -> String {
        let mut code = String::from("sequenceDiagram\n");
        for i in 0..10 {
            code.push_str(&format!("participant P{i}\n"));
        }
        for i in 0..n / 2 {
            code.push_str(&format!(
                "alt case {i}\nP{} ->> P{}: request {i}\nNote over P{}: note {i}\nelse other\nP{} -->> P{}: reply {i}\nend\n",
                i % 10,
                (i + 3) % 10,
                i % 10,
                (i + 3) % 10,
                i % 10
            ));
        }
        code
    }

    const SHAPES: &[Shape] = &[
        ("diamonds", diamonds),
        ("bipartite", bipartite),
        ("complete", complete),
        ("fan", fan),
        ("tree", tree),
        ("ring", ring),
        ("pipeline", pipeline),
        ("class", class_dense),
        ("state", state_dense),
        ("er", er_dense),
        ("requirements", requirements),
        ("packets", packets),
        ("messages", messages),
        ("framed messages", framed_messages),
    ];

    /// Whether the layout limit lets the diagram through.
    fn affordable(code: &str) -> bool {
        let parsed = parse_mermaid_strict(code).expect("parse");
        layout_is_affordable(code, &parsed.graph)
    }

    /// The most edges of a shape that the limit lets through.
    fn largest_affordable(make: fn(usize) -> String) -> usize {
        let mut n = 4;
        while n < 5000 && affordable(&make(n + n / 8 + 1)) {
            n += n / 8 + 1;
        }
        n
    }

    #[test]
    fn diagrams_at_the_limit_render_within_a_few_seconds() {
        for &(name, make) in SHAPES {
            let n = largest_affordable(make);
            let code = make(n);
            let start = std::time::Instant::now();
            // The library checks its own layout in debug builds (these tests)
            // and panics when a route for one of these contrived shapes grazes
            // a node; release builds, which ship, do not check, and the
            // bridge turns a panic into an error anyway. Time is what is
            // measured here.
            let result = match std::panic::catch_unwind(|| raster(&code, "auto", 1.0)) {
                Ok(result) => result,
                Err(panic) => {
                    let message = panic
                        .downcast_ref::<String>()
                        .map(String::as_str)
                        .or_else(|| panic.downcast_ref::<&str>().copied())
                        .unwrap_or_default();
                    assert!(message.contains("flowchart stage"), "{name}: {message}");
                    Err("diagram too large".to_owned())
                }
            };
            let took = start.elapsed();
            eprintln!("{name}: {n} edges, {} bytes, {took:?}", code.len());
            // (A very tall diagram can still be refused for its pixel count.)
            assert!(result.is_ok() || result.as_ref().err().map(String::as_str) == Some("diagram too large"), "{name}: {:?}", result.err());
            // About 1.5 s on the machine it was measured on; the margin is
            // for slow or busy CI machines. Unbounded layouts take minutes.
            // Debug builds (`cargo test`) also run the library's own layout
            // checks and leave this crate unoptimised: 7 s on Windows CI.
            let limit = if cfg!(debug_assertions) { 20.0 } else { 6.0 };
            assert!(took.as_secs_f64() < limit, "{name} with {n} edges took {took:?}");
        }
    }

    #[test]
    fn slow_diagrams_are_refused_before_the_layout() {
        for &(name, make) in SHAPES {
            let code = make(1000);
            assert!(code.len() < MAX_SOURCE_BYTES, "{name}: {}", code.len());
            let start = std::time::Instant::now();
            assert_eq!(raster(&code, "auto", 1.0).err().as_deref(), Some("diagram too large"), "{name}");
            assert!(start.elapsed().as_secs_f64() < 2.0, "{name} took {:?}", start.elapsed());
        }
    }

    #[test]
    fn a_300_node_flowchart_still_renders() {
        // A pipeline of 300 steps with a decision every tenth step.
        let mut code = String::from("flowchart TD\n");
        for i in 0..300 {
            if i % 10 == 9 {
                code.push_str(&format!("N{i}{{Check {i}?}} -->|yes| N{}[Step {}]\nN{i} -->|no| E{i}([Failed {i}])\n", i + 1, i + 1));
            } else {
                code.push_str(&format!("N{i}[Step {i}] --> N{}\n", i + 1));
            }
        }
        let start = std::time::Instant::now();
        let (w, h, pixels, _) = raster(&code, "auto", 1.0).expect("render");
        eprintln!("300 nodes: {w}x{h} in {:?}", start.elapsed());
        assert_eq!(pixels.len(), (w * h * 4) as usize);
        assert!(start.elapsed().as_secs_f64() < 6.0, "took {:?}", start.elapsed());
    }

    #[test]
    fn ordinary_diagrams_are_not_limited() {
        // Mermaid's own medium-sized examples, a few dozen edges each.
        let flowchart = "flowchart LR\n A[Start] --> B{Is it?}\n B -->|Yes| C[OK]\n C --> D[Rethink]\n D --> B\n B -->|No| E[End]\n";
        assert!(affordable(flowchart));
        assert!(affordable(&pipeline(300)));
        assert!(affordable(&tree(60)));
        assert!(affordable(&messages(500)));
        assert!(affordable(&class_dense(100)));
        assert!(affordable("pie\n \"a\" : 1\n \"b\" : 2\n"));
        assert!(affordable(&format!("gantt\ndateFormat YYYY-MM-DD\n{}", "T :t, 2024-01-01, 1d\n".repeat(2000))));
    }

    #[test]
    fn many_participants_are_limited() {
        let participants = |n: usize| {
            (0..n).fold(String::from("sequenceDiagram\nP0 ->> P1: hi\n"), |code, i| code + &format!("participant P{i}\n"))
        };
        assert!(affordable(&participants(1500)));
        assert!(!affordable(&participants(4000)));
    }

    #[test]
    fn linear_kinds_are_limited_by_lines() {
        let journey = |tasks: usize| {
            (0..tasks).fold(String::from("journey\n title T\n section S\n"), |code, i| code + &format!(" Task {i}: 3: Me\n"))
        };
        assert!(affordable(&journey(2900)));
        assert!(!affordable(&journey(3100)));
    }
}
