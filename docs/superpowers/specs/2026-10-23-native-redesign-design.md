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

## New defaults (first launch)

| Setting | Value |
|---|---|
| Font | JetBrains Mono, bundled (SIL OFL 1.1) on macOS and Linux; Cascadia Mono on Windows |
| Font size | 13 pt on macOS; 12 pt on Windows and Linux |
| Terminal padding (margins) | 8 horizontal × 6 vertical |
| Window size | 100 × 30 |
| Colors | `contour-dark` + `contour-light`, following the system; today's palette survives as `contour-classic` |
| Status line | off |
| Tab bar | shown from the second tab (`Multiple`) |
| Links | decorated only while the follow modifier is held (`Modifier`) |
| Cursor | block, steady |
| Unfocused split dim | 0.15 |
| Bell | no sound; alert on |
| Scrollback | 10 000 lines |
| GUI theme | `terminal`: chrome follows the terminal background's luminance |

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
