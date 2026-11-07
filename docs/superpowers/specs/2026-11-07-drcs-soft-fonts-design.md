# Downloadable soft fonts (DECDLD/DRCS), finished — design

Issue: [#467](https://github.com/contour-terminal/contour/issues/467) — "[VT] Add support for downloadable
soft fonts". Status: design, awaiting approval.

## Intent

DECDLD support landed in `4e565829f` (parsing, Dscs designators, SCS designation, DA1 ext 7, reset), and #467
was left open. Reading it against DEC's VT510 manual and Windows Terminal's `FontBuffer.cpp` (James
Holderness's implementation, the one #467 points at) shows that **no soft font found in the wild renders
today**. All ten sample fonts collected for this work fail on the first parameter alone. The goal is to finish
the feature so that #467 can be closed: real fonts load, look right at Contour's cell size, behave like text,
and work wherever a Contour session runs, daemon-hosted sessions included.

The work comes from walking the issue board, so done means a feature that works properly, not a demo.

## What exists, and what is wrong with it

| # | Defect | Where |
|---|--------|-------|
| 1 | `Pcn` is read as a code point (`Pcn=1` stores the glyph at key 1), but it is a *position* in the set, so `Pcn=1` means 2/1 (`!`). Every real font uses `Pcn=1`, and its glyphs are never found. Today's tests pass only because they use `Pcn=0`. | `Terminal::defineDRCS` |
| 2 | Sixel columns past the assumed width are not bounded and spill into the next pixel row. `Pw`/`Pss` (80/132 columns × 24/36/48 lines) is read as a pixel width. `Pcmw` 2/3/4 (the 5×10, 6×10 and 7×10 matrices) are read as pixel widths. When unspecified, the size falls back to an invented 10×20. | `Terminal::defineDRCS`, `Screen::hookDECDLD` |
| 3 | There is no detection of the cell size the font was designed for, so glyphs are scaled with aspect preserved (`ResizeToFit`, `TopStart`) against a wrong height. | `Terminal::createDRCSImage` |
| 4 | A standard designator wins over a loaded soft set. The vt100.net fonts all use Dscs `B`, so `ESC ( B` selects ASCII and never the font. DEC: "the most recently loaded set is used". Windows Terminal checks the DRCS id first. | `Screen::tryHandleSCS` |
| 5 | `Pe=2` (erase every soft set) is treated like `Pe=0`. `Pcss=1` (96-character sets) is ignored. `Pt` (text vs full cell) is ignored. `Pfn` is not range-checked. | `Terminal::defineDRCS` |
| 6 | Each write bakes the glyph into a new RGBA image in the *current* foreground colour, so the glyph ignores later SGR, reverse video, palette changes and selection. Each write also allocates a fresh image and GPU texture. The cell's text is a space, so copy and search see blanks. | `Screen::writeSoftFontGlyph` |
| 7 | Definitions never reach a daemon-attached client. It only receives the colour-baked image. | `src/vthost` (no DRCS anywhere) |

8-bit input is **already** handled: since `ae203fc60` a raw 0x90 starting a character is DCS, and a raw 0x9C
on a UTF-8 boundary is ST. The vt100.net `.fnt` files (`0x90 … 0x9C`, CR/LF inside the data) need no parser
change, only defects 1 and 4 fixed and a test that proves it.

## Goals

1. **Correct DECDLD semantics** per DEC STD 070 / the VT510 manual, cross-checked against Windows Terminal's
   `FontBuffer.cpp` and `terminalOutput.cpp`. All the defects in rows 1, 2, 4 and 5 are fixed.
2. **Target-cell-size detection** ported from Windows Terminal's `FontBuffer::_calculateDimensions`. It
   infers which terminal the font was made for (VT2xx 10×10 with text clipped to 8, VT240 6×10, VT320 15×12 /
   9×12, VT420/VT5xx 10×16 / 6×16, VT340 10×20 / 6×20, VT382 12×30 / 7×30, the 36- and 48-line sizes)
   from the declared matrix, `Pss`, `Pt`, and the extent of the sixel data actually received. Text fonts are
   clipped to their text width and centred in the cell, as in Windows Terminal's `_packAndCenterBitPatterns`.
3. **Soft-font glyphs render like monochrome text glyphs.** The renderer keeps one alpha mask per glyph,
   scaled to the cell and coloured at rasterization time from the cell's real attributes: foreground, SGR,
   reverse video, hidden, palette, selection and search highlights. This is the user's requirement. It is the
   model `BoxDrawingRenderer` already follows: an `atlas::Format::Red` tile, cached by identity rather than
   colour, with colour applied in `text.frag`.
4. **Crisp scaling.** The user's requirement is that scaled glyphs stay crisp. Glyphs are scaled
   nearest-neighbour, independently on each axis, to fill the cell exactly. No smoothing or filtering.
5. **Cells carry their real characters.** Copy, search, accessibility and anything else reading cell text
   see the character the application sent (for example `A` for the glyph loaded at `A`). This is the user's
   requirement.
6. **Redefinition is live.** Cells refer to *(font, position)*, not to a frozen bitmap, so redefining a
   glyph changes every cell already showing it, as on DEC hardware and in Windows Terminal.
7. **Daemon mode works.** This is the user's requirement. Soft-font definitions and per-cell glyph references
   cross the native protocol, and an attached client renders them through the same mask path.
8. **Live upgrade carries them.** The state codec already stores `drcsCharsets` and `drcsDesignators`, and
   the new per-cell references must round-trip too.
9. **Proven on real fonts.** Tests are built from fragments of real fonts (see *Verification*), and an
   offscreen render is checked by eye.

## Non-goals

- Multiple renditions per font number (separate 80-column, 132-column and bold buffers, as on a real VT510).
  As in Windows Terminal, one definition per font number is kept and its original target is used only to
  scale it.
- Synthetic bold or italic of soft glyphs. The mask is drawn as-is, and only the colour follows SGR.
- tmux control mode (`tmux -CC`). Pane output there is fed into local terminals as bytes, so it gets soft
  fonts for free and needs no work here.
- Changing how non-soft-font input is decoded, including DOCS. 8-bit C1 handling stays exactly as it is.

## Anti-goals (would make this a failure even if it "works")

- Blurry or smoothed glyphs.
- Glyphs that keep their write-time colour after SGR, reverse video or selection change.
- A measurable slowdown of ordinary text. The soft-font check on the write path is already an inline load,
  thanks to `582854fb4`. It must stay that way, and `bench-headless` instruction counts on `ascii`, `utf8`
  and `tui` must not move.
- Per-write allocation. A glyph is stored once per definition, not once per cell.
- Lines that never use a soft font paying memory or a shape downgrade for the feature.

## Decided by the user

- Glyphs are monochrome masks coloured during rasterization (goal 3).
- Scaled glyphs stay crisp (goal 4).
- Copy and search yield the underlying characters (goal 5).
- Daemon-hosted and attached sessions are in scope (goal 7).
- Pasting a raw 8-bit `.fnt` file must work (covered by existing parser support, plus a test).
- **A cell whose soft font no longer exists** (after RIS, `Pe=2`, or a reload under a different font
  number) shows its underlying character as ordinary text, scrollback included. Only positions left undefined
  in a font that *is* loaded show the error glyph. So `clearDRCS` and `Pe=2` need not walk the grid.

## Calls made in writing this (check these)

- **Live redefinition** (goal 6), following DEC and Windows Terminal.
- **Undefined positions in a loaded set** show DEC's error glyph, a reverse question mark, scaled the same
  way. DEC: "Erased characters are undefined (not blank). The terminal displays these characters as the error
  character." Windows Terminal does the same (`_fillUnusedCharacters`). Today Contour falls back to the
  ASCII character. That fallback came about incidentally (`6fc79a3b6` fixed single-shift consumption around
  it) and was not a ruling, so it changes. Copy and search still yield the underlying character.
- **Fill the cell exactly** rather than scale by whole multiples, so full-cell fonts (line art, cmatrix's
  glyphs) join up between cells.
- **`Pfn` accepts 0–2**, as DEC specifies. Other values ignore the sequence. Contour keeps one definition per
  font number. The current map already allows several.
- **`Pcn=0` in a 94-character set** follows Windows Terminal: position 2/0, which a 94-set never displays, so
  the first pattern is effectively unused. Existing tests that rely on `Pcn=0` meaning `!` are corrected.
- **Double-width and double-height lines** (DECDWL/DECDHL) scale the glyph across the doubled cell, as
  `BoxDrawingRenderer` does through its line flags.
- **Older daemon clients** that do not negotiate the new capability see the underlying characters as plain
  text, not images. The colour-baked image path is removed rather than kept as a fallback.

## Shape of the solution

The builder makes the detailed calls. This is the agreed outline:

- **`vtbackend` font model.** Replace `DRCSGlyph`/`DRCSCharset` and `defineDRCS`'s inline sixel loop with a
  small, pure, table-testable font builder (Windows Terminal's `FontBuffer` is the reference): parameter
  validation, sixel accumulation, dimension detection, packing and centring, error glyph. It lives in the
  `vtbackend` layer where it fits (`vt/` decoders or `graphics/`). It has no dependency on `Screen`.
  Each defined glyph is immutable once built (see *Thread boundary*).
- **Grid.** A cell written through a soft set stores its real codepoint, plus a reference to *(font,
  position)* in a sparse per-line side table beside `imageFragments` (`LineSoA`). Ordinary lines pay nothing.
  A line holding such a reference must degrade its shape exactly as image fragments do, or the trivial-line
  fast path drops it. `writeSoftFontGlyph` stops calling `createDRCSImage`, which goes away.
- **Render buffer.** `RenderCell` gains the soft-glyph reference, and `RenderBufferBuilder::makeRenderCell`
  copies it.
- **`vtrasterizer`.** A `SoftFontRenderer`, a sibling of `BoxDrawingRenderer`, called from
  `TextRenderer::renderCell` ahead of the cluster grouper. It uploads a cell-sized `Red` tile, keyed by
  glyph identity and generation, into the shared atlas, and colours it at draw time. Font-size changes
  rebuild the atlas already (`Renderer::applyPendingReconfig` → `configureTextureAtlas`), so scaled tiles
  regenerate for free. The glyph arrives in the `RenderCell` (see the thread boundary
  below), so the renderer needs no access to `Terminal`.
- **Thread boundary (a call made after review).** `RenderBufferBuilder` runs under the state lock, while
  `TextRenderer` draws afterwards without it. Each defined glyph is therefore an immutable
  `shared_ptr<const SoftGlyph>`, and redefinition replaces the pointer rather than mutating it (copy-on-write).
  `makeRenderCell` resolves *(font, position)* to that pointer under the lock, as `RenderCell::image`
  already does, and the renderer works purely from the `RenderCell`. No interface is injected into
  `Renderer`, and glyph identity serves as the generation and the atlas key. Live redefinition shows on the
  next frame. A TSan `[threading]` test covers redefinition racing rendering.
- **`vthost`.** A capability-gated PDU carries soft-font definitions (the packed bitmaps plus their target
  size), sent on attach and on change. A cell side-table entry analogous to `ImageCellEntry` carries the
  per-cell references. `RemoteScreen`/`ScreenMirror` keeps the definitions and feeds the same render path.
- **State codec.** Persist the new definition model and the per-cell references, and keep the existing
  live-upgrade tests green.

## Verification

- **Unit tests** (`vtbackend_test`): parameter decoding, `Pcn`/`Pcss` positions, erase controls, the
  designator precedence, error glyph, dimension detection per target terminal (table-driven, one row per
  terminal size), text clipping and centring, live redefinition, copy and search text.
- **Real-font fixtures**, small fragments checked into the test tree with their source noted: a vt100.net
  `.fnt` (raw 8-bit DCS, `Pt=2`, Dscs `B`), a larsbrinkhoff/sixel VT220 font (`P1;1;1{ @`), and cmatrix's
  `P1;1;2{ @` font. Collected so far: https://vt100.net/dec/vt320/ (`boothill.fnt` and siblings),
  https://github.com/larsbrinkhoff/sixel, https://github.com/jhamby/cmatrix. The other sources James listed
  in #467 are welcome extras.
- **`vtrasterizer_test`**: the soft-font tile is an alpha mask, scaled nearest-neighbour to the cell
  (`bitmapRows`), cached once across many cells, and coloured per cell.
- **Daemon** (`ctest -L daemon`): definitions and references mirror exactly. `GridParity` is extended.
- **Visual**: an offscreen render (`QT_QPA_PLATFORM=offscreen`, grabbed to PNG) of the real fonts, compared
  by eye against the vt100.net previews.
- **Performance**: `bench-headless` instruction counts before and after, which must be unchanged for
  non-soft-font workloads.
- Zero warnings under `-Werror`. `ctest --preset=clang-asan` green.

## Left to the builder

The font builder's exact location and names. The PDU layout and capability name. Whether the side table
holds a pointer or an index. Test file organisation. How the work is split
into commits.
