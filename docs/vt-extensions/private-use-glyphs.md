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
| `U+10F002` | BLOCK RUNNING | A small filled triangle pointing right, as `U+25B8` ▸. |
| `U+10F003` | USER MARK | A filled diamond, as `U+25C6` ◆. |
| `U+10F004` | BLOCK FINISHED | A filled disc, as `U+25CF` ●. |
| `U+10F005` | BLOCK FAILED | A diagonal cross, as `U+2717` ✗. |
| `U+10F006` | FOLD BODY | A vertical line through the cell, in the stem's column, as `U+2502` │. |
| `U+10F007` | FOLD BODY END | A vertical line from the cell's top to its middle, turning right to the cell's edge, as `U+2514` └. |
| `U+10F008` | FOLD HEAD OPEN HEAVY | `U+10F000` drawn at twice the stroke. |
| `U+10F009` | FOLD HEAD CLOSED HEAVY | `U+10F001` drawn at twice the stroke. |
| `U+10F00A` | FOLD BODY HEAVY | `U+10F006` drawn at twice the stroke. |
| `U+10F00B` | FOLD BODY END HEAVY | `U+10F007` drawn at twice the stroke. |
| `U+10F00C` | FOLD HEAD OPEN HEAVIER | `U+10F000` drawn at three times the stroke. |
| `U+10F00D` | FOLD HEAD CLOSED HEAVIER | `U+10F001` drawn at three times the stroke. |
| `U+10F00E` | FOLD BODY HEAVIER | `U+10F006` drawn at three times the stroke. |
| `U+10F00F` | FOLD BODY END HEAVIER | `U+10F007` drawn at three times the stroke. |

The fold column is drawn as line art at the current foreground colour, scaled to the cell: the box is
square whatever the cell's aspect ratio, and every glyph of the column shares one stroke and one column,
so that the head's stem, the body and its end meet exactly. The stroke is the cell's own line weight --
twice it for the heavy four, three times for the heavier four -- even where the gutter draws its glyphs
two cells wide.

The four marks are filled shapes at the current foreground colour, anti-aliased, centred in the cell
and spanning 60% of its shorter side, so they sit on the same line as the fold column's stem.

### What they are for

They form the head of Contour's **fold column** — the gutter drawn to the left of the grid, which
marks each foldable [OSC 133](osc-133-shell-integration.md) command block:

```
⊟─ $ make -j8          <- U+10F000, the block is open
│    [  1/120] cc a.c  <- U+10F006
│    [  2/120] cc b.c
└    ... done          <- U+10F007, the block's last row

⊞  $ make -j8          <- U+10F001, the block is collapsed
```

The body and its end look like `U+2502` (BOX DRAWINGS LIGHT VERTICAL) and `U+2514` (BOX DRAWINGS LIGHT
UP AND RIGHT), and once were. They are Contour's own because of the two-cell gutter: a box-drawing
character drawn two cells wide takes the doubled stroke of a DECDWL row, whose text is stretched to twice
its width, while the head beside it does not -- so the body stepped out of the stem's weight at every head.
Drawn here, the column has one stroke in either width.

The head needs a codepoint of its own in any case. `U+229F` SQUARED MINUS and `U+229E` SQUARED PLUS are the box and its sign, but
neither carries the stem that joins an open head to the body beneath it, and a head that did not
connect would read as two unrelated marks rather than as one bracket down the side of a block.

A box carrying a plus or a minus, rather than a triangle, because that is the fold control a reader
has already met in editors, tree views and file managers.

The marks share that column. A command that is still running heads its block with `U+10F002`, a
finished one with no fold control beside it with `U+10F004` -- or `U+10F005` when it failed -- and a row
the user marked (vi `mm`) carries `U+10F003`. A failed command whose output folds draws its whole fold
heavy, head and body, with `U+10F008` to `U+10F00B`, so failure reads by shape as well as by colour --
collapsed too, where only the head is drawn. The fold under the pointer is drawn one step heavier again
(`gutter.bold_on_hover`): heavy for a light fold, `U+10F00C` to `U+10F00F` for a failed one.

The gutter's block column may be two cells wide (`gutter.block_column_width`); its glyphs are then drawn
across both cells, each twice the size.

Unicode has these four shapes at `U+25B8`, `U+25C6`, `U+25CF` and `U+2717`, but the gutter does not use them:
the renderer's claim on a codepoint is global, so claiming those would draw every one a program
writes -- the ● of `systemctl status`, a prompt theme's ▸ or ◆, DEC Special Graphics' `` ` `` -- as a
gutter mark instead of in its font's own size and weight.

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
