//! Mermaid diagrams: mermaid-rs-renderer lays the diagram out as SVG and
//! resvg rasterizes it to PNG for Qt.

use std::sync::{Arc, OnceLock};

use mermaid_rs_renderer::{RenderOptions, Theme, render_with_options};
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

/// Renders a diagram to premultiplied RGBA8 pixels at the given scale (device
/// pixel ratio); a diagram too big for that scale is drawn at a smaller one.
/// Returns the width, height, pixels and the scale actually used.
pub fn raster(code: &str, theme_name: &str, scale: f32) -> Result<(u32, u32, Vec<u8>, f32), String> {
    if code.len() > MAX_SOURCE_BYTES {
        return Err(String::from("diagram too large"));
    }
    let theme = theme(theme_name);
    let svg = render_with_options(code, RenderOptions { theme: theme.clone(), ..RenderOptions::default() })
        .map_err(|e| e.to_string())?;
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
}
