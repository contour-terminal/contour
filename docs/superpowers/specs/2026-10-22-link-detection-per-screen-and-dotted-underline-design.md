# Per-screen link detection and an even dotted underline

Date: 2026-10-22 · Branch: `feature/link-detection-per-screen` · One PR for both parts.

## Why

Two irritations in how printed links look and behave:

1. **Printed file paths are detected everywhere.** In full-screen applications (vim, less, htop, a TUI file
   manager) the text on screen is the application's own rendering, not command output, and paths in it get
   underlined, hovered and probed. That is noise, and the detection work (scanning, path probing) is wasted.
   On the primary screen the user wants it on by default but switchable.
2. **The dotted underline looks bad under text.** Today each cell draws two hard-edged square dots, one at
   x=0 and one at `width / 2` (`src/vtrasterizer/DecorationRenderer.cpp`, `Decorator::DottedUnderline`). On
   odd cell widths the gaps alternate short/long, and the pattern restarts at every cell. The user wants the
   dots to look homogeneous across the grid.

## Part 1: Automatic link detection, configurable per screen

### Scope

- Only **automatic** detection: links found by scanning printed text (`LinkDetector`, the passive
  hover/decoration/click path).
- **OSC 8 hyperlinks are always links**, on every screen, regardless of any setting here. Nothing in this
  change touches them.
- **Hint mode is not affected** (my call): it is an explicit user gesture, not automatic detection, so it keeps
  offering URLs and paths on every screen whatever these switches say. It is the one exemption.
- **Every other consumer of the detector follows the switches** (decided by the user): hover, decoration,
  tooltip, the follow click, the on-demand lookup behind a click and the context menu's link items
  (`Terminal::lookUpLinkAt()`), and the vi link motions `]x` / `[x` (`LinkDetector::scanNow()`). So the gate
  sits in the shared matching/classification step, not only in the background collect, and an on-demand scan
  on a page whose kind is off neither matches nor probes it.

### Screens

Two groups: the **primary screen** (page 0) and **every other screen** — the alternate screen and any
further page (VT420 pages, today `ScreenType::Alternate` / `screenTypeFromPage()`). The "other screens"
setting must apply to *all* non-primary pages, not just the alternate screen, and must keep applying if more
display pages are added later. Decide by "page is primary or not", never by "page is the alternate screen".

### Settings

Two kinds × two screen groups, per profile:

| | URLs | File paths |
|---|---|---|
| Primary screen | on | on |
| Other screens | on | **off** |

Out of the box the only behaviour change is that paths stop being detected on non-primary screens.

YAML, in the profile next to `hyperlink_decoration` and `hint_patterns` (key names are my call; rename freely):

```yaml
link_detection:
  primary_screen:
    urls: true
    file_paths: true
  other_screens:
    urls: true
    file_paths: false
```

- "URLs" means `LinkKind::Url`; "file paths" means `LinkKind::Path`. A `file://` URL printed in text is
  treated as a path by the existing classification (`classifyPrintedLink`), so it follows the **file paths**
  switch. User-defined `hint_patterns` (`LinkKind::Custom`) follow the **URLs** switch (my call — they open
  through a URL template).
- Missing keys take the defaults above; a profile that inherits from another inherits these like any other
  profile field.
- `Config`'s fields are `bool` at the YAML boundary (the documented exception in AGENT.md); inside
  `vtbackend` the policy is carried as a small value type (e.g. a per-screen-group set of enabled `LinkKind`s),
  not as loose bools threaded through signatures.
- Documented in `ConfigDocumentation.hpp` and the user docs under `docs/configuration/`, with the defaults.

### Engine behaviour

- The policy lives in `vtbackend` (`Settings`), applied where `LinkDetector` collects/matches for the page on
  display, so decoration, hover, tooltip, pointer shape and the follow gesture all agree, and the daemon /
  remote paths get the same behaviour.
- Switching screens (e.g. entering vim) takes effect on the next frame: the index is already rebuilt on a page
  switch (`LinkIndexKey`), so the new page is scanned under its own group's switches.
- When both kinds are off for the page on display, no collect is scheduled at all — that is the optimization,
  not merely hiding decorations. When only URLs are on, path candidates are dropped before any path probing.
- Live config reload applies the new policy the same way `setLinkPatterns` replaces the detector today.

### Settings page

- The four switches appear in the Settings page's profile pane. Preferred home: the existing
  "Links: opening and patterns" section (`linksSection` in `SettingsPage.qml`), as a small
  screen-group × kind grid of checkboxes above the pattern list. If the profile-field descriptor machinery
  (`profileFieldGroups()` in `SettingsController.cpp`, which supports dotted keys) is the cleaner way to get
  saving, search and read-only handling for free, four `bool` fields with dotted keys
  (`link_detection.primary_screen.urls`, …) are acceptable instead — builder's call, as long as they are
  findable by the settings search and save through the same overrides path as other profile fields.
- Changes apply live to open sessions.

## Part 2: An even, round dotted underline

Chosen from mockups: **round, antialiased dots on one continuous pitch across the line** (option "D").

- Dot diameter `d = max(2, round(1.5 × underline thickness))`; pitch `2d` (dot, then an equal gap).
- Dots are discs with antialiased edges (coverage, not hard squares), vertically centred on the underline
  position, kept inside the cell like the current code's clamping does (a font whose underline sits near the
  cell bottom must not make the dots vanish).
- **Continuous across cells:** the pattern's phase is taken from the absolute pixel x (column × cell width),
  not restarted per cell. Because decorations are direct-mapped atlas tiles, one tile per decorator is not
  enough: the dotted underline needs one tile per phase, `pitch / gcd(cellWidth, pitch)` of them, chosen per
  column at render time. These are rebuilt whenever grid metrics change (font size, DPI), as tiles are today.
- Applies everywhere `Decorator::DottedUnderline` is drawn: SGR 4:4 dotted underline text and the hyperlink
  decoration (default `hyperlink_decoration.normal: dotted-underline`). Double-width lines keep working.
- Other decorators are unchanged.

## Non-goals

- No change to OSC 8 handling, hint mode, link pattern syntax, or the link opener.
- No new decoration styles; no per-screen decoration styles.
- No change to what counts as a URL or a path.

## Anti-goals (would be a failure even if it "works")

- Paths still being probed on a non-primary screen while merely not drawn.
- A setting that only covers the alternate screen and silently misses other pages.
- Dots that still visibly restart or bunch up at cell boundaries at any common cell width (7–20 px) and
  thickness (1–3 px).
- `bool` parameters threaded through `vtbackend` APIs for the policy.

## Testing

- `vtbackend_test`: for each screen group × kind combination, a printed URL and a printed path are or are not
  detected/decorated; a non-primary page other than the alternate screen follows the "other screens" switches;
  OSC 8 links are unaffected by every combination; no collect is scheduled when both kinds are off; hint mode
  still finds paths on the alternate screen; `]x` / `[x` and the context menu's link lookup do not find paths
  there under the defaults, and do find them when `other_screens.file_paths` is on.
- Config tests: defaults, parsing, round-trip writing, profile inheritance.
- Settings controller test (`contour_gui_test`): the switches show the profile's values and save.
- `vtrasterizer` test: the dotted tiles' dot positions form an evenly spaced sequence across consecutive cells
  for odd and even cell widths and thicknesses 1–3; dots stay inside the tile.

## Delivery

One branch (`feature/link-detection-per-screen`, in its own worktree), one PR covering both parts, with a
release note entry in the metainfo, a clean `-Werror` build, and the suite passing under `clang-asan`.
