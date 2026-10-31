# Visual Notepad design

Visual Notepad (`visnotepad`) is a native, lightweight replacement for
Notepad with first-class Markdown editing and preview. It replaces
VisualMarkdown, a trimmed VS Code/Electron fork, and must reach parity with
that app's Markdown editing and preview features.

Targets: Windows and Linux (Linux Mint 22.3 is the reference desktop), on
x86-64 and ARM64. The app should look and behave the same on both systems.

## Scope

In:

- Markdown: source mode, side-by-side source/visual, and a hybrid visual
  editor; outline; GFM, math, front matter, Mermaid; links; formatting.
- Any text file, xed-style: syntax highlighting only.
- Multiple cursors, VS Code-style find/replace, customizable shortcuts.

Out: extensions/VSIX, theme installation, command palette, language
servers, completion, diagnostics and other IDE features.

## Stack

| Part | Choice | Notes |
|---|---|---|
| UI | Qt 6 Widgets (C++20), Fusion style | Qt shipped as shared libraries (LGPLv3) |
| Highlighting | KSyntaxHighlighting (KDE Frameworks 6) | Code blocks and text files |
| Markdown parsing | comrak, in the Rust `vn-core` crate | GFM, math, front matter, source positions |
| Mermaid | mermaid-rs-renderer (Rust, rasterized with resvg) | |
| Math | MicroTeX, Qt backend | User macros via `\newcommand` |
| Formatter | comrak | Edit > Format Document |
| C++/Rust glue | cxx, built by Corrosion | `src/` includes `vn_core_bridge/lib.h` |

The hybrid editor is a port of `@vscode/markdown-editor` 0.0.2-119 (MIT),
whose TypeScript was recovered from the npm package's source maps. The
parser, commands, model and diff logic port directly; the browser view is
rewritten for Qt. The model is extended from one selection to a list of
selections for multiple cursors. One editor core serves Markdown and plain
text. Find/replace (replace, replace all, preserve case, history) follows
VS Code's `editor/contrib/find`. Ported Microsoft code keeps its MIT notice.

## Look

Theme "Light Grey Warm" is the only theme and is not exposed in settings.
Colours derive from the Windows 2000 grey `#D4D0C8`; surfaces are flat, with
no bevels. Values live in `src/Theme.h`.

| Surface | Colour |
|---|---|
| Editor, text inputs | `#EFEEEB` |
| Active block | `#F6F5F3` |
| Menu bar, menus, dropdowns, Preferences pane | `#E9E7E2` |
| Code block | `#E4E2DD` |
| Side bar, inactive tab | `#DDDAD4` |
| Status bar, tab strip | `#D4D0C8` |
| Panel title | `#CBC7BD` |
| Dividers | `#BAB5AB` |
| Muted text | `#7C766A` |
| Accent (navy) | `#0A246A` |
| Selection, menu hover | `#CFD5E3` with navy text |
| Switch on | `#6F82AD` |

## Layout

- VS Code layout under the native OS title bar. Dialogs are separate OS
  windows with OS title bars.
- Menu bar (File, Edit, Selection, View, Help) inside the window on every
  desktop; no command palette.
- Outline: left panel, Markdown only, toggled by View > Outline or Ctrl+Shift+B;
  hidden for other files.
- Tab strip, right end: Split Editor (opens an empty right group) and a mode
  dropdown showing an icon plus the current mode. Items: Source,
  Source / Visual, Visual Editor; no icons, a check on the current one. The
  default mode is a setting.
- Lock/pencil pill (read-only vs edit) floats top-right, below the tab strip.
- Find bar: VS Code style, docked top-right of the editor.
- Tabs can be dragged out into a new window; also a tab menu item
  "Move to New Window" (Ctrl+K O).
- Status bar, Notepad-style right-aligned columns: position, selections,
  zoom, line ending, encoding, language (clickable), sizing grip.
- Web links always prompt: Open, Copy, Cancel.

## Preferences

xed-style dialog under Edit > Preferences (Ctrl+,). Left category list:
Editor, Markdown, Save, View, Shortcuts. Bold headings, indented rows,
switches. Buttons: Help and Close; changes apply immediately. No search box.

- Shortcuts: click a row and press keys; warns on conflicts; per-row reset
  and Reset All.
- Markdown > Math macros: a Command / Expands to table (`\RR` to
  `\mathbb{R}`), as the old `markdown.math.macros` setting.
- Markdown > Images: the size of the decoded-picture cache
  (`performance/pictureCacheMiB`, 16-2048 MiB, default 128), applied live to
  all windows; pictures are only kept while their document is open.

## Shortcuts

Follow VS Code's Windows/Linux defaults, with Notepad touches: Ctrl+Q quits,
Ctrl+0 resets zoom. On Linux, Format Document is Ctrl+Shift+I, Add Cursor
Above/Below is Shift+Alt+Up/Down, Copy Line is Ctrl+Shift+Alt+Up/Down.

## Open questions

- Raw HTML in Markdown: Qt's rich text supports only a subset.

## Milestones

1. Window, menus, tabs, status bar; multi-cursor editor core with
   highlighting. *Done.*
2. Find/replace bar, Preferences with custom shortcuts. *Done.*
3. Visual Editor and Source / Visual modes. *Done.*
4. Math (MicroTeX) and Mermaid (mermaid-rs-renderer). *Done.*
5. Links, encodings, session restore, editor groups and tab dragging,
   Markdown conveniences. *Done; needs testing on a real desktop.*
6. Packaging for Windows and Linux, x86-64 and ARM64. *Deferred until
   the user is satisfied with the app.*

The Markdown parser is comrak (markdown-rs was 170x slower on large
files). MicroTeX's res/greek and res/cyrillic fonts are GPLv3; review
before distribution.
