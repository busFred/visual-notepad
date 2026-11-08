# Visual Notepad design

Visual Notepad (`visnotepad`) is a native, lightweight replacement for
Notepad with first-class Markdown editing and preview. It replaces
VisualMarkdown, a trimmed VS Code/Electron fork, and must reach parity with
that app's Markdown editing and preview features.

Targets: Windows and Linux (Linux Mint 22.3 is the reference desktop), on
x86-64 and ARM64, and macOS 11 or later on Intel and Apple Silicon. The app
should look and behave the same on every system.

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
- Remote images (http/https) never load unasked. A bar above the visual view
  lists the hosts and the number of images and offers Load images / Don't
  load, for the session and per document; Don't load is final until the file
  is reopened. Preferences > Markdown > Remote images can be set to Never
  load. The bar counts what Qt would actually request (entities, background
  attributes and CSS `url()` included). Downloads are limited (20 MiB,
  5 redirects, never https to http, 10 s stall timeout, 4 at a time, at
  most 200 distinct addresses a session, no cookies, a generic User-Agent
  without the version), run off the GUI thread (Qt's network thread, and a
  worker thread decodes each picture once) and are cached in memory only
  (64 MiB as downloaded, 32 MiB decoded). Like local pictures they shrink to
  the column, open in the zoom viewer and count toward the render cache; an
  SVG that names a file on another machine is refused, as a local one is.
- Remote images never come from this machine or a local network: loopback
  (127/8, ::1), private (10/8, 172.16/12, 192.168/16, fc00::/7), link-local
  (169.254/16, which holds cloud metadata services, fe80::/10), carrier-grade
  NAT (100.64/10), unspecified and 0/8, multicast, reserved and broadcast,
  documentation and benchmarking ranges, IPv6 site-local and anything outside
  2000::/3; IPv4 inside IPv6 (mapped, compatible, NAT64, 6to4) is judged by
  its IPv4 address, Teredo is refused. Odd IPv4 spellings (`2130706433`,
  `0x7f000001`, `0177.0.0.1`, `127.1`) are read the way resolvers read
  them. Names `localhost`, `*.localhost`, `*.localdomain`, `*.local`,
  `*.internal`, `*.lan`, `*.home.arpa` and single-label names are refused
  without a lookup. Every other host, the first and each redirect's, is
  looked up before the request and refused if any of its addresses is not
  public; the request then goes to the address that was checked, with the
  name kept in the Host header and for TLS (SNI and the certificate check),
  over HTTP/1.1. Such pictures look like failed ones and the bar leaves them
  out; its tooltip says local-network pictures are never loaded.
  Remaining gaps: with a system proxy in use the proxy connects by name and
  looks it up itself, so a name that answers our lookup with a public address
  and the proxy's with a local one (DNS rebinding) reaches whatever the proxy
  can reach; and only one checked address is tried, so a host whose first
  address (IPv4 preferred) is unreachable fails rather than trying the next.
- Links that would run a program warn first (Open Anyway / Don't Open,
  default Don't Open, full path shown, never remembered): local files the OS
  would launch (`md::isLaunchable`: a deny-list of program, script,
  installer and shortcut suffixes, judged as Windows reads names, plus
  execute-bit files that are not plain documents) and every scheme other
  than http, https, mailto and file. Text files (scripts too) open in the
  app; network paths (UNC, `file://host`) are refused outright.

## Visual editor

The hybrid editor shows every block rendered and is typed into like a word
processor (`VisualView`, "Rendered editing").

- A click puts the caret where it lands in the rendered text; the block
  stays rendered. Links and task boxes act on the click. A double click
  opens the block as Markdown source, the caret at the clicked spot (in
  source a double click selects a word). The block closes again when the
  caret moves out of it, on Esc, or on a click in a side margin, on a blank
  line or below the end. Losing the focus shows it rendered meanwhile.
  Read-only views open nothing.
- Typing in rendered text goes into the source where the caret renders: at
  the edge of bold, italic, code, strikethrough or link text it takes the
  formatting of the character before the caret (at the start of a block or
  line, of the one after). `` * _ # ` [ ] < | \ ~ `` (and `$` with math on)
  are inserted escaped, as are `- + > =` and the dot or parenthesis of
  `1.` at the start of a line, so they show as typed. Text typed on a blank
  line becomes a paragraph of its own.
- Enter starts a new paragraph; in a list, a new item with the same marker
  (numbered on); in a quote, a new paragraph in it; in a heading, a
  paragraph after the heading. Backspace and Delete take one rendered
  character (with its escape) and remove markup they leave empty (`****`,
  `[](url)`); at the start of a list item (the end of one) they join it to
  the item before (after): its marker and box go. A selection in one block
  (mouse or Shift+arrows) is replaced or deleted the same way, and Format >
  Bold and the like wrap it. Arrows, Home and End move through the rendered
  text, from block to block over blank lines, without opening anything.
- Every edit is checked first: the block, parsed and rendered with it, must
  show what was typed (white space aside, typographic quotes and dashes as
  typed). What fails that, or cannot be done in rendered text (a selection
  over several blocks, joining two blocks, several carets, Ctrl+Backspace,
  Shift+Enter, Tab, an input method composing, typing in a table, code or
  front matter) opens the block as source and is done there. Math, diagrams,
  pictures and raw HTML take no typing until opened with a double click, as
  where it would go is unknown. A block over 2000 characters or 40 lines
  (a long list: an item) opens as source when typed into: it renders again
  at every keystroke, about 1 ms a KiB for a paragraph.
- A block edited in rendered text renders again at once from a parse of its
  own source (only that block; its link definitions and footnotes elsewhere
  show after the document's reparse, 120 ms after typing stops). Undo and
  redo work as in source, the caret drawn where it renders.
- A long top-level list (16 items or 2 KB of HTML on) shows an item a row:
  each item renders on its own (`RenderedBlock::createItem`) and the view
  draws its bullet or number as Qt draws a list's (`drawListItem`), so
  typing in an item, Enter or a join renders that item alone and the items
  after are only renumbered; only the rows on screen are laid out. It looks
  as the whole list does (Qt's margins between items, the same heights and
  line positions; markers round to pixels from the item's top, within half
  a pixel of the whole list's), except that an empty item shows its marker
  on an empty line where Qt drops it. Edits since the last parse are kept
  as line ranges: the items they touch are parsed again on their own (and
  must stay items of the list: same bullet or delimiter and indentation),
  the others keep the parse's rendering, also after the reparse when their
  HTML is unchanged. A list shows whole if an item cannot render alone as
  it does in the list: raw HTML other than the core's tags or out of
  balance in an item, an item opening with a list or a code block of
  several lines, a table.

## Table editing

Pipe tables (GFM) can be edited without touching the pipes. The work is text
to text, in `core/src/table.rs` (rows, columns, alignment, sorting,
transposing, one cell; the result is always aligned like Format Document
does), called through `vn::md::tableEdit`.

- Format > Table > Insert Table, and the same submenu in the context menu of
  any editable Markdown view: a grid of 8 x 10 squares (pointing picks rows,
  counting the header, by columns; the arrow keys and Enter work too), and
  Insert Table of Custom Size... for a larger table (a Rows x Columns
  dialog, up to 200 x 50). The table commands below are enabled while the
  menu is open, from the caret's table; the context menu has them inside a
  table. No default shortcuts.
- In the visual editor a new table stays rendered and opens its first
  header's cell box, its text selected to type over; when nothing follows
  the table, a blank line and a line to type on are added after it.
- In the Markdown views, Tab and Shift+Tab move between cells (selecting the
  cell's text), Tab in the last cell adds a row, and Enter adds a row below.
  Nothing is rewritten while moving or typing.
- Visual editor: a table holding the caret shows a floating toolbar (rows,
  columns, alignment, a More menu) above it, and is realigned once the caret
  has left it after an edit (one undo step; the other views leave tables
  alone until Realign).
- Visual editor, editable: a click on a cell of a rendered table opens a
  text box over that cell with its Markdown source instead of opening the
  whole table as source. Enter, clicking elsewhere or leaving the view
  writes the cell back (a `|` is escaped, line breaks become `<br>`) and
  realigns the table; Esc drops the edit; Tab, Shift+Tab, Up and Down
  continue in another cell (Tab after the last cell adds a row). The table
  toolbar and the Table menu act on that cell. A table is edited this way
  only if the block is exactly one pipe table; to edit its source, move the
  caret into it with the keys and type. Read-only views do not open the box.
- Visual editor, editable: cells, rows and columns of a rendered table
  (pipe or HTML; not read-only views) are selected as in a word processor.
  A thin band above the table's columns points a down arrow (a cursor
  drawn from a small pixmap), and one left of its rows a right arrow; a
  click there selects that column or row, a drag along it several, Shift+click
  extends. Dragging from one cell to another selects the rectangle between
  them (a click without a drag still opens the cell box; a drag from a cell
  no longer selects text). The selection is painted over the table in the
  theme's selection colour; Esc, a click elsewhere or the next edit drops it,
  Delete or Backspace clears the cells' text. The context menu over a
  selection offers Insert N Rows Above/Below, Delete N Rows, Insert N Columns
  Left/Right, Delete N Columns, Align Columns, Clear Cells and then the Table
  menu (singular for one); the toolbar's buttons act on the selection too.
  Each is one undo step, made by one edit in the core
  (`table::edit_range`: rows and columns counted from the first or last
  selected; Delete Rows skips a pipe table's header, and all the body rows
  leave one empty; the last column stays). The row and column commands are
  off for HTML tables with merged cells. Where two rows meet at the table's
  left edge, or two columns at its top edge, a plus and a line across the
  table show; a click inserts a row or column there. Nothing is looked up
  while pointing but the table's cell edges, found once for each render
  (`RenderedBlock::tableGrid`); only the mark and the selection's rectangle
  are painted again.
- A table is found by the same rules as the formatter (header and delimiter
  lines with as many cells, rows with a leading and closing pipe, quote and
  list prefixes, not inside fenced code). Rows with fewer cells get empty
  ones; extra cells are dropped, as GFM does. The lookup reads only the run
  of pipe-ended lines around the caret (kept across edits in it) and a list
  of the document's fence lines, so a keystroke in a long table or a long
  document stays cheap; whether a table over 500 lines is aligned is not
  checked (Realign stays enabled for it). An edit replaces only the lines
  that differ from the table as it was (a realign after typing in one row
  is a change of that row, unless the row became the widest of a column).
- HTML tables (`core/src/html_table.rs`): a `<table>` that starts a line
  (after at most three spaces) in an HTML block, outside fenced code, is
  edited with the same commands, toolbar and cell box. An edit changes only
  what it must in the source (attributes, comments, indentation and the
  layout of the rows are kept; a new row or cell copies its neighbour's
  layout, with empty cells); end tags HTML lets out (`</td>`, `</tr>`) are
  understood. Row 0 is a header only if it is in `<thead>` or all `<th>`.
  Alignment is an `align` attribute on each cell of the column (a
  `text-align` in a `style` is taken out). A cell is written as typed (it is
  HTML, Markdown is not read inside an HTML block); a line break becomes
  `<br>`, as a blank line would end the block. Tables with merged cells
  (`colspan`, `rowspan`) only have their cells' text edited; Transpose
  writes the table anew. Realign does not apply. Tab and Enter keep their
  usual meaning in HTML source. The cell box opens on a rendered HTML block
  that shows nothing but the table (it may be inside a `<div>`).
- Convert to HTML Table / Convert to Pipe Table: a pipe table becomes a
  `<table>` with `<thead>`/`<tbody>` (each cell's Markdown rendered to HTML;
  not for tables in lists or quotes; a blank line is added after it when
  text follows), and an HTML table without merged cells or `<pre>` whose
  block is only the table becomes a pipe table (white space collapsed, `|`
  escaped, row 0 the header).

## Preferences

xed-style dialog under Edit > Preferences (Ctrl+,). Left category list:
Editor, Markdown, Save, View, Shortcuts. Bold headings, indented rows,
switches. Buttons: Help and Close; changes apply immediately. No search box.

- Shortcuts: click a row and press keys; warns on conflicts; per-row reset
  and Reset All.
- Markdown > Remote images: Ask before loading (default) or Never load.
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
