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

/// Renders a diagram to PNG at the given scale (device pixel ratio).
pub fn png(code: &str, theme_name: &str, scale: f32) -> Result<Vec<u8>, String> {
    let theme = theme(theme_name);
    let svg = render_with_options(code, RenderOptions { theme: theme.clone(), ..RenderOptions::default() })
        .map_err(|e| e.to_string())?;

    let options = usvg::Options { fontdb: fonts(), font_family: theme.font_family.clone(), ..Default::default() };
    let tree = usvg::Tree::from_str(&svg, &options).map_err(|e| e.to_string())?;
    let scale = scale.clamp(0.5, 4.0);
    let size = tree.size().to_int_size();
    let width = ((size.width() as f32) * scale).ceil() as u32;
    let height = ((size.height() as f32) * scale).ceil() as u32;
    let mut pixmap = Pixmap::new(width.max(1), height.max(1)).ok_or("diagram too large")?;
    resvg::render(&tree, Transform::from_scale(scale, scale), &mut pixmap.as_mut());
    pixmap.encode_png().map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_flowchart() {
        let png = png("flowchart LR; A-->B", "auto", 1.0).expect("render");
        assert!(png.starts_with(&[0x89, b'P', b'N', b'G']));
    }

    #[test]
    fn reports_syntax_errors() {
        assert!(png("not a diagram %%%", "default", 1.0).is_err());
    }
}
