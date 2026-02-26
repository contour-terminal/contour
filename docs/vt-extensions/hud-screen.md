# HUD Screen

Terminal applications sometimes need to display popup-like overlays (menus, tooltips, floating windows)
on top of existing screen content. The HUD (Heads-Up Display) screen provides a transparent overlay layer
that composites on top of the primary screen without modifying its contents.

This extension is controlled via a DEC mode (`2035`).

## Semantics

When HUD mode is enabled:

- A clean overlay screen is created with the same dimensions as the primary screen.
- All subsequent VT output is directed to the HUD screen instead of the primary screen.
- During rendering, the primary screen is drawn first, then HUD screen cells with content
  are composited on top. Empty (unwritten) HUD cells are transparent, showing the primary
  screen through.
- The cursor position is governed by the HUD screen.

When HUD mode is disabled:

- The HUD overlay is discarded entirely.
- The primary screen becomes the active screen again, fully intact.
- No cleanup of the HUD content is needed — disabling the mode suffices.

Each time the mode is enabled, the HUD screen starts in a clean (freshly reset) state.

The HUD mode only operates on top of the primary screen. Enabling it while the alternate
screen is active has no effect. If the alternate screen is activated while HUD is active,
the HUD is automatically disabled first.

## Feature detection

Use `DECRQM` (`CSI ? 2035 $ p`) to detect support for the HUD screen mode.

## Using the feature

Use `CSI ? 2035 h` to enable the HUD screen overlay.

Use `CSI ? 2035 l` to disable the HUD screen overlay.

## Example

```
# Write primary screen content
printf "Hello, World!\n"
printf "Background text.\n"

# Enable HUD and draw a popup box
printf "\033[?2035h"
printf "\033[5;10H┌──────────┐"
printf "\033[6;10H│  Popup!  │"
printf "\033[7;10H└──────────┘"

# ... user interacts with the popup ...

# Disable HUD — popup disappears, primary screen intact
printf "\033[?2035l"
```

## Adoption State

| Support | Terminal/Toolkit/App | Notes |
|---------|----------------------|-------|
| ✅      | Contour              |       |
