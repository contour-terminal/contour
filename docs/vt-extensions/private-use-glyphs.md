# Private Use Glyphs

Contour draws a few glyphs that Unicode has no character for. Rather than inventing a second
drawing channel for them, they are given codepoints in a Private Use Area and rendered by the same
built-in box-drawing renderer that draws `U+2500`–`U+257F`, the block elements and the legacy
computing symbols.

This page documents those codepoints so that other terminals, fonts and tools can render the same
glyphs if they want to. Nothing here is a protocol: no escape sequence introduces these characters,
and Contour never requires an application to emit one.

## The reserved block

**`U+10F000`–`U+10F0FF`** — Contour Terminal private glyphs.

The block sits in **Supplementary Private Use Area-B** (plane 16) rather than in the BMP's
`U+E000`–`U+F8FF`. That lower range is comprehensively claimed by Nerd Fonts — Powerline, Font
Awesome, Devicons, Octicons, Seti, Material and others — and Contour already renders several of
those assignments itself (`U+E0B0`–`U+E0BE`, `U+EE00`–`U+EE05`, `U+F5D0`–`U+F60D`).

It also sits away from the *start* of a plane. An allocation that outgrows the BMP tends to land at
the beginning of the next area it is offered, which makes `U+F0000` — the first codepoint of plane
15 — one of the more contended places to put a block, not one of the safer ones. `U+10F000` is near
the far end of plane 16 instead, where no widely-deployed assignment reaches.

Only the codepoints listed below are actually claimed by the renderer. The rest of the block is
reserved for future use and deliberately falls through to the font, so that reserving a range costs
nothing to anyone who is already using it.

## Assigned codepoints

| Codepoint | Name | Glyph |
|---|---|---|
| `U+10F000` | FOLD HEAD OPEN | A square box containing a minus, with a stem descending from its bottom edge to the bottom of the cell. |
| `U+10F001` | FOLD HEAD CLOSED | A square box containing a plus. No stem. |

Both are drawn as line art at the current foreground colour, scaled to the cell: the box is square
whatever the cell's aspect ratio, and the stem shares its column with `U+2502` so that the two meet
exactly.

### What they are for

They form the head of Contour's **fold column** — the gutter drawn to the left of the grid, which
marks each foldable [OSC 133](osc-133-shell-integration.md) command block:

```
⊟─ $ make -j8          <- U+10F000, the block is open
│    [  1/120] cc a.c
│    [  2/120] cc b.c
┕    ... done          <- U+2515, the block's last row

⊞  $ make -j8          <- U+10F001, the block is collapsed
```

The body and tail need no private codepoints — `U+2502` (BOX DRAWINGS LIGHT VERTICAL) and `U+2515`
(BOX DRAWINGS UP LIGHT AND RIGHT HEAVY) already describe them exactly.

The head does. `U+229F` SQUARED MINUS and `U+229E` SQUARED PLUS are the box and its sign, but
neither carries the stem that joins an open head to the body beneath it, and a head that did not
connect would read as two unrelated marks rather than as one bracket down the side of a block.

A box carrying a plus or a minus, rather than a triangle, because that is the fold control a reader
has already met in editors, tree views and file managers.

## Notes for implementers

- These characters are **produced by the terminal, for its own gutter**. They are not part of any
  escape sequence, and an application has no reason to write one.
- A Private Use Area assignment is by definition not exclusive. Planes 15 and 16 are also used by
  some East Asian fonts for user-defined characters (GB 18030 and Big5 user-defined areas), so a
  document that already uses `U+10F000` for something else will render as a fold head in Contour.
  Turning the gutter off (`folding.show_markers: false`) does not change that, because the glyphs
  are claimed by the renderer rather than by the fold feature — if this ever proves to matter in
  practice, the claim is one entry in `BoxDrawingRenderer::renderable()`.
- If you implement these in a font, note that Contour will still prefer its own rendition: the
  box-drawing renderer is consulted before font shaping, and `font.builtin_box_drawing: false`
  turns that off for all built-in glyphs, not just these.
