# Native redesign: design record

The visual design lives on a canvas of 18 boards:
<https://claude.ai/artifact/6cp7tcSnurTSMbhSDePWqH>. This file records the decisions those boards
encode, so that implementation plans can cite them from inside the repository.

## Decisions (owner-approved)

- **Platforms.** macOS comes first, then Windows 11, then **one** Linux design that serves every
  desktop. There is no GNOME-specific or KDE-specific variant.
- **One component model.** Platform differences live in UiStyle token rows: `MacOS`, `Fluent`,
  `ContourLinux` and `Terminal` (TUI). The user-facing `ui_style: native` resolves to the row for the
  host platform; `ui_style: terminal` keeps the cell-quantized TUI row.
- **Chrome versus content.** Glass and material appear on chrome only, never behind terminal text.
  The chrome's light or dark appearance follows the terminal background.
- **Tabs** live in the title bar. Settings is a separate window (⌘, / Ctrl+,). Menus are native on
  macOS and Windows.
- **Panes.** A single pane runs edge to edge with no focus frame. Pane cards with headers appear
  only once a tab is split, and focus is shown by dimming, not by an outline.
- **Densities.** There are two: Comfortable and Minimal.
- **Command blocks** never draw UI over non-blank terminal cells. The status colour for "running"
  is the accent colour, never yellow (yellow means search).
- **Shortcuts render per platform:** ⌃⌥⇧⌘ glyphs on macOS, `Ctrl+Shift+P` elsewhere.

## Coverage additions (owner-approved 2026-10-24)

A coverage audit checked every QML surface in `src/contour/qml`, `src/contour/styles/ContourTui`,
the in-grid overlays and PR #2176 (merged as 4637e4a6) against the canvas. It added four boards: *Terminal (TUI)
style*, *Daemon & remote hosts*, *Status line* and *Every other surface*. The redesign builds on
PR #2176: it restyles that PR's surfaces and keeps their behaviour and models.

- **Terminal (TUI) style** is a first-class row of the same components. Every extent counts in
  cells and the chrome uses the terminal font. Its colours come from the active terminal scheme,
  not the OS palette:
  - surface = background, bars and headers = palette[0], secondary = bright black;
  - accent = blue, danger = red, success = green;
  - warning = yellow, as a glyph only and never as a fill.

  The TUI row also differs from the native rows in four ways:
  - menus stay in-scene on every OS;
  - the only shadow is a hard one-cell offset;
  - the find bar lives in the pane header row;
  - Minimal hides the bar row while there is one tab.

  Every phase lands the TUI row together with the native rows. On macOS the TUI row keeps AppKit's
  real traffic lights, snapped to the first cells of the bar row; Linux CSD and Windows draw
  "● ● ●" cells from the scheme.
- **Daemon and remote** (PR #2176). Its literals move to tokens in Phase 0 Task 6; the restyle
  lands in Phase 1:
  - The host badge becomes the kit's HostChip (connected · Reconnecting… · Disconnected — Reconnect).
  - The pane reconnecting overlay dims by the pane-dim token and shows a toast-style pill.
  - The hosting-fallback notice becomes a Banner below the title band.
  - The drift badge moves from bottom-right over the cells to a toolbar chip in the title band.
  - OSC 7501 program status shares one tab **badge slot** with the unseen-output dot and the
    finished-command badge. Priority is blocked › error › working › done › output. "Working" uses
    the accent, never green.
  - Attach to Host is a palette drill-in and a Shell submenu.
  - Control and extension consent is a two-button Dialog with a "Remember" checkbox.
- **Status line** is **on by default**, cell-drawn and scheme-coloured. Its new default template is
  `{InputMode:Hide=Insert}{Context}{Cwd}{Git}`, then
  `{LastCommand}{Selection}{SearchPrompt}`, then `{Progress}{Scroll}{Clock:Faint}`.
  - New placeholders: `{Cwd}`, `{Git}`, `{LastCommand}`, `{Selection}`, `{Scroll}`.
  - The new `Hide=` flag and the role colours (`Color=accent|success|danger|warning|muted`) land
    in Phase 3.
  - Phase 0 turns it on, takes its colours from the scheme (the `indicatorStatusLine*` palette
    pairs of contour-dark and contour-light), and drops the `#0270C0` and `#FFFF00` literals and
    `{Tabs}` from the default, using today's placeholders only.
- **Remaining surfaces** each get a "will be": the tab and title-bar context menus, the tab colour
  flyout and the new-tab profile menu; the five permission walls, which become one Dialog whose two
  buttons and checkbox replace the five-button prompt and the `(bool allow, bool remember)` pair;
  Save Layout; hint labels, the size indicator, the scrollbar and tooltips; no title bar and full
  screen; bell, dock and taskbar.
- **Defaults, complete list** (canvas board *Every visible default*). Every visible `Config{}`
  default was reviewed: 22 are kept as they are, and these join the table below:
  - `chrome_density: comfortable`;
  - `tab_label` falls back to the process name when the title is empty;
  - the 80 ms cursor glide is dropped under Reduce Motion;
  - `hyperlink_hover_tooltip: false`, because the ⌘-held target pill replaces it;
  - an overlay scrollbar that hides at the bottom;
  - the screen transition is 150 ms, and none under Reduce Motion;
  - `option_as_alt` stays off but becomes per side (Off / Left / Right / Both).

## New defaults (first launch)

| Setting | Value |
|---|---|
| Font | JetBrains Mono, bundled (SIL OFL 1.1) on macOS and Linux; Cascadia Mono on Windows |
| Font size | 13 pt on macOS; 12 pt on Windows and Linux |
| Terminal padding (margins) | 8 horizontal × 6 vertical |
| Window size | 100 × 30 |
| Colors | `contour-dark` + `contour-light`, following the system; today's palette survives as `contour-classic` |
| Status line | on, scheme-coloured (see Coverage additions) |
| Tab bar | shown from the second tab (`Multiple`) |
| Links | decorated only while the follow modifier is held (`Modifier`) |
| Cursor | block, steady |
| Unfocused split dim | 0.15 |
| Bell | no sound; alert on |
| Scrollback | 10 000 lines |
| GUI theme | `terminal`: chrome follows the terminal background's luminance |
| Chrome density | `comfortable` |
| Tab label | `{WindowTitle}`, falling back to the process name |
| Hyperlink hover tooltip | off (the ⌘-held target pill replaces it) |
| Scrollbar | overlay on the right, hidden at the bottom |
| Screen transition | fade, 150 ms; none under Reduce Motion |
| Cursor motion | 80 ms glide; none under Reduce Motion |
| Option as Meta | off, configurable per side (Off / Left / Right / Both) |

### contour-dark

- bg `#1C1E24`, fg `#D9DCE3`, cursor `#E8C26E`, selection `#3A4A6B`.

| Colors | black | red | green | yellow | blue | magenta | cyan | white |
|---|---|---|---|---|---|---|---|---|
| Normal | `#2A2D35` | `#F0757A` | `#8FCB7A` | `#E6C06B` | `#6FA8F5` | `#C79BF2` | `#5FC8C4` | `#C5CAD3` |
| Bright | `#848DA2` | `#FF9095` | `#A8DE94` | `#F2D48A` | `#93BEFF` | `#D9B5FF` | `#84DDD8` | `#F4F6FA` |

### contour-light

- bg `#FAFAF8`, fg `#2B2E35`, cursor `#1F5FBF`, selection `#C8D8F5`.

| Colors | black | red | green | yellow | blue | magenta | cyan | white |
|---|---|---|---|---|---|---|---|---|
| Normal | `#2B2E35` | `#B42D35` | `#2A7A1F` | `#8A6100` | `#1F5FBF` | `#8A3FB0` | `#12757A` | `#5F6670` |
| Bright | `#6B7280` | `#C9343D` | `#277F1C` | `#8F6400` | `#2B6FD6` | `#9B4CC2` | `#0F7A80` | `#3A3F48` |

### Contrast requirement

Every colour except black and white must be at least 4.5:1 against its scheme's background, and
foreground against background must also reach 4.5:1. Bright black on dark is allowed at 5.0.

## Phases

0. **Foundations**: token rows, colours, defaults, shortcut text, focus frame. This plan.
1. **macOS window**: expanded client area and safe area, tab well, menus.
2. **Panes**: cards and headers, the two densities.
3. **Overlays**: palette, find bar, toasts, dialogs.
   - 3b. **Command blocks**.
4. **Settings window.**
5. **Windows and Linux rows.**
6. **Moments**: first run, restore, quick terminal, and similar.
