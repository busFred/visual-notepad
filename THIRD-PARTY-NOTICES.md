# Third-party notices

Visual Notepad's own code is under the MIT license (see `LICENSE`). It is
built on the components below. Binary packages also contain the license
texts of every bundled library in their `licenses/` folder.

## Linked libraries

| Component | License | Source |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Svg, platform plugins) | LGPL-3.0-only | <https://code.qt.io/cgit/qt/>; the packages use conda-forge's `qt6-main` build |
| KDE KSyntaxHighlighting 6.31 | MIT (library code); syntax definition files carry their own licenses (see below) | <https://invent.kde.org/frameworks/syntax-highlighting/-/tree/v6.31.0> |
| MicroTeX (math rendering) | MIT | <https://github.com/NanoMichael/MicroTeX/tree/0e3707f6dafebb121d98b53c64364d16fefe481d> |
| TinyXML-2 10.0.0 | Zlib | <https://github.com/leethomason/tinyxml2/tree/10.0.0> |
| ICU | Unicode-3.0 | <https://github.com/unicode-org/icu> |
| Rust crates in `core/` (comrak, mermaid-rs-renderer, resvg, cxx and their dependencies) | MIT, Apache-2.0, BSD-2/3-Clause, Zlib, Unicode-3.0, Unlicense (each crate's own choice) | `core/Cargo.lock` lists the exact versions |

Qt is linked dynamically. You may replace the bundled Qt libraries with your
own build of the same version; the source for the version in a package is
available from the link above, or from the author on request.

## KSyntaxHighlighting syntax definitions

KSyntaxHighlighting compiles its syntax definition files into its library.
Most are MIT; some are under the LGPL or GPL. Each file states its license
in its header.
The complete source of the library, including every definition, is at the
link above.

## Fonts

The interface font is Segoe UI when it is installed. Segoe UI may not be
redistributed, so the app embeds **Selawik** 1.01, Microsoft's open-source
stand-in for it, and uses that otherwise. Selawik is under the SIL Open Font
License 1.1 (`resources/fonts/Selawik-OFL.txt`); source:
<https://github.com/microsoft/Selawik/releases/tag/1.01>.

The editor font is Consolas when it is installed. Consolas may not be
redistributed either, so the app embeds **Inconsolata** (SemiExpanded
Regular and Bold), which was modelled on Consolas and has the same character
width, and uses it otherwise. Inconsolata is under the SIL Open Font License
1.1 (`resources/fonts/Inconsolata-OFL.txt`); source:
<https://github.com/google/fonts/tree/main/ofl/inconsolata>.

## MicroTeX resources

The math fonts shipped under `microtex/res/fonts` are under the SIL Open Font
License, Knuth's license for the Computer Modern fonts, and the dsrom font
license; their texts are in `microtex/res/fonts/licences`. MicroTeX's
`res/greek` and `res/cyrillic` packages are GPL-3.0 and are **not** included.

## Bundled system libraries (packages only)

The Linux and Windows packages bundle the libraries the app needs at run time
(for example FreeType, HarfBuzz, fontconfig, libpng, zlib, libjpeg-turbo,
PCRE2, GLib, libxkbcommon). Their licenses (FreeType License, MIT, zlib, IJG,
BSD, LGPL-2.1-or-later and others) are included with each package.
