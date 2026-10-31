//! Bounded gzip decompression, so that a compressed SVG (`.svgz`) can be
//! checked the way a plain one is before the SVG plugin opens it.

use std::io::Read;

use flate2::read::MultiGzDecoder;

/// The contents of gzip data, or an empty `Vec` if the data is not valid gzip
/// or would be longer than `limit` bytes (a decompression bomb).
///
/// All members are decoded, as QtSvg does (it resets zlib at the end of a
/// member and goes on while input remains), so that what is checked is what
/// QtSvg will parse.
pub fn gunzip_bounded(data: &[u8], limit: usize) -> Vec<u8> {
    let mut out = Vec::new();
    // One byte more than allowed tells "exactly limit" from "too long".
    let read = MultiGzDecoder::new(data).take(limit as u64 + 1).read_to_end(&mut out);
    if read.is_err() || out.len() > limit {
        return Vec::new();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::Compression;
    use flate2::write::GzEncoder;
    use std::io::Write;

    fn gzip(data: &[u8]) -> Vec<u8> {
        let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(data).unwrap();
        encoder.finish().unwrap()
    }

    #[test]
    fn round_trips() {
        let text = b"<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        assert_eq!(gunzip_bounded(&gzip(text), 1024), text);
    }

    #[test]
    fn all_members_are_decoded() {
        let mut data = gzip(b"<svg>");
        data.extend(gzip(b"</svg>"));
        assert_eq!(gunzip_bounded(&data, 1024), b"<svg></svg>");
    }

    #[test]
    fn the_limit_is_inclusive() {
        let data = gzip(&[b'a'; 100]);
        assert_eq!(gunzip_bounded(&data, 100).len(), 100);
        assert!(gunzip_bounded(&data, 99).is_empty());
    }

    #[test]
    fn a_decompression_bomb_is_refused_without_being_expanded() {
        // 1 GiB of zeros is a few MiB of gzip.
        let mut encoder = GzEncoder::new(Vec::new(), Compression::fast());
        let block = vec![0u8; 1 << 20];
        for _ in 0..1024 {
            encoder.write_all(&block).unwrap();
        }
        let bomb = encoder.finish().unwrap();
        assert!(bomb.len() < 16 << 20, "{}", bomb.len());
        let start = std::time::Instant::now();
        assert!(gunzip_bounded(&bomb, 32 << 20).is_empty());
        assert!(start.elapsed().as_secs() < 5, "took {:?}", start.elapsed());
    }

    #[test]
    fn malformed_data_gives_nothing() {
        assert!(gunzip_bounded(b"<svg/>", 1024).is_empty());
        assert!(gunzip_bounded(&[], 1024).is_empty());
        let mut data = gzip(b"<svg/>");
        data.extend_from_slice(b"trailing garbage");
        assert!(gunzip_bounded(&data, 1024).is_empty());
        let truncated = &gzip(&[b'x'; 1000])[..12];
        assert!(gunzip_bounded(truncated, 4096).is_empty());
    }
}
