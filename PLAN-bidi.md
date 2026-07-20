# Full RTL / BiDi support for Contour (Hebrew & Arabic as first-class citizens)

## Context

Contour today has **no bidirectional text support at all**. What exists is honest scaffolding:
`DECMode::RightToLeftMode` (DECRLM, 34), `HebrewEncodingMode` (DECHEM, 36) and
`RightToLeftCopyMode` (DECRLCM, 96) are settable and reported truthfully by DECRQM, but
`src/vtbackend/primitives.h:921-946` says outright that **nothing reads the bit**, and calls the
bidirectional pair "the priority of this group". `docs/internals/vt-conformance.md:130-136` and
`docs/internals/text-stack.md:175-178` both record BiDi as the known, deliberate gap.

Meanwhile every layer hard-codes left-to-right:

- `open_shaper.cpp:494` — `hb_buffer_set_direction(hbBuf, HB_DIRECTION_LTR)`, unconditionally.
  `hb_buffer_guess_segment_properties()` runs after and cannot override an explicitly set direction.
- `mapScriptToHarfbuzzScript()` (`open_shaper.cpp:323-335`) maps only Latin/Greek/Common; Arabic and
  Hebrew fall through to `HB_SCRIPT_INVALID`.
- `directwrite_shaper.cpp:379,440,483` passes `isRightToLeft = 0` and `bidiLevel = 0`;
  `directwrite_analysis_wrapper.h:62-66` returns `DWRITE_READING_DIRECTION_LEFT_TO_RIGHT` with a
  `// TODO: is this always correct?`, and `SetBidiLevel()` is a no-op stub — DirectWrite is
  *offering* resolved levels and Contour discards them.
- No logical↔visual column mapping exists anywhere: cursor placement, mouse hit-testing
  (`helper.cpp:178-181`), selection ranges and `SelectionRenderer`'s line-break inference all assume
  column order == visual order.

The outcome we want: type, display, select, copy and navigate Hebrew and Arabic correctly — correct
reordering, correct Arabic contextual joining, correct mirrored brackets, a cursor that lands where
the user expects, and mouse/selection that map back to logical positions.

## The standard we implement

The de-facto standard is the **terminal-wg BiDi recommendation**
(<https://terminal-wg.pages.freedesktop.org/bidi/>), which VTE implements. Its load-bearing
principles:

- **BiDi is display-only.** The grid stores logical order; escape sequences address the *model*.
  Reordering happens on the way to the screen, and mouse reports undergo the inverse transform.
- **A paragraph is delimited by hard newlines, not by rows.** Wrapped lines continue the paragraph.
  Contour already tracks this as `LineFlag::Wrapped` (`Line.h:245`).
- **The cursor sits *over* a character, not between characters** — so it simply follows its
  character, and only its side (left for LTR, right for RTL) and shape hint change.

Escape sequences it defines (all to be added):

| Sequence | Meaning | Default |
|---|---|---|
| `CSI 8 h` / `CSI 8 l` | **BDSM** — implicit (terminal does BiDi) / explicit (app does BiDi) | implicit |
| `CSI Ps SP k` | **SCP** — character path: `1`=LTR, `2`=RTL, `0`/omitted=restore default | 0 |
| `CSI ? 2500 h/l` | mirror box-drawing glyphs in RTL context | reset |
| `CSI ? 2501 h/l` | autodetect paragraph direction (first strong character) | reset |
| `CSI ? 1243 h/l` | swap Left/Right arrow keys inside RTL paragraphs | **set** |

Existing `DECRLM` (34) is wired as an alias for RTL paragraph direction; `DECHEM` (36) and
`DECRLCM` (96) get real behaviour and lose their TODOs.

## Decisions taken

- **UAX#9 lives in libunicode**, not FriBidi/ICU — no new third-party dependency (AGENT.md), and
  libunicode already carries `Bidi_Class`, `Bidi_Paired_Bracket_Type`, `is_mirrored()` and
  `bidi_mirroring_glyph()`, plus UCD 17.0.0 locally **including `BidiTest.txt` and
  `BidiCharacterTest.txt`** for full conformance testing.
- **Arabic shaping: HarfBuzz first, presentation-form fallback.** Shape RTL runs with
  `HB_DIRECTION_RTL` + the real script tag so the font's GSUB supplies `init/medi/fina/isol` and
  lam-alef; fall back to U+FE70–U+FEFF presentation forms only when the resolved font has no Arabic
  GSUB coverage.
- **Work happens in `D:\libunicode`** on a branch, consumed here via
  `-DCPM_libunicode_SOURCE=D:/libunicode` (CPM v0.38.3 supports this), and lands as its own PR.
- **One branch, `feature/rtl-text`, phased commits** — each phase below is one reviewable,
  bisectable commit.
- **Reference checkouts go to `D:\others`**, alongside the existing `alacritty`, `refterm`, `tabby`.

---

## Phase 0 — Reference sources and fonts

**Clone into `D:\others`** (the tree name in the first column is what tooling resolves):

| Subdir | Upstream |
|---|---|
| `mlterm` | <https://github.com/arakiken/mlterm> |
| `vte` | <https://gitlab.gnome.org/GNOME/vte> |
| `fribidi` | <https://github.com/fribidi/fribidi> (read as a UAX#9 cross-check, not linked) |

Then set `CONTOUR_VT_REFERENCE_SOURCES` to `D:/others` in `.claude/settings.local.json` (git-ignored)
— the convention `docs/internals/vt-conformance.md:222-229` documents. Note `D:\others` already holds
terminal trees but none of the ones `vt-conformance.md` tabulates; adding these three does not
disturb that.

**Fonts — per-user install, no elevation needed.** This machine already does exactly this: JetBrains
Mono NFM lives in `%LOCALAPPDATA%\Microsoft\Windows\Fonts` and is registered under
`HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts`. Same mechanism, same two steps: copy the
file, add the registry value. DirectWrite's system font collection picks up per-user fonts, so
Contour sees them without a logout.

Already present and covering both scripts: Courier New (monospace), Arial, Times New Roman, Tahoma,
Segoe UI. So fallback works even today — but none is a good BiDi test bed. Install (all OFL/free):

| Font | Why |
|---|---|
| **DejaVu Sans Mono** | the key one: one *monospace* font covering Hebrew **and** Arabic, so end-to-end tests need no fallback |
| **Noto Naskh Arabic** | rich Arabic GSUB — joining forms and lam-alef ligature |
| **Noto Sans Hebrew** | Hebrew with niqqud (combining marks over base) |
| **Amiri** | stress-tests Arabic ligature/mark stacking |
| **Scheherazade New** (SIL) | second independent Arabic shaping reference |
| **Miriam Mono CLM** (Culmus) | monospace Hebrew, cross-check against DejaVu |

Also worth enabling if the user is willing to elevate once: the Windows *Arabic* and *Hebrew
Supplemental Fonts* optional features (`Get-WindowsCapability -Online`) — not required, since the
list above covers everything we test.

Verify afterwards by shaping U+0627 U+0644 U+0639 U+0631 U+0628 U+064A U+0629 and U+05E9 U+05DC U+05D5 U+05DD
through each font and confirming non-`.notdef` glyphs.

---

## Phase 1 — libunicode: BiDi properties + UAX#9 (separate PR)

Repo: `D:\libunicode`, branch `feature/bidi`. Two independently testable pieces.

**1a. Surface the properties.** `codepoint_properties` (`src/libunicode/codepoint_properties.h:27`)
carries script, width, grapheme break, word break … but **no bidi class**. Add
`Bidi_Class bidi_class` and `Bidi_Paired_Bracket_Type bidi_paired_bracket_type` fields to the packed
struct and populate them in `codepoint_properties_loader.cpp` from `DerivedBidiClass.txt` /
`BidiBrackets.txt`. Keep `std::has_unique_object_representations_v` holding. Add
`bidi_paired_bracket(char32_t)` as a generated function next to the existing
`bidi_mirroring_glyph()` in `tablegen/ucd_api_generator.cpp:520-564` (same shape, sourced from
`BidiBrackets.txt`) — a sparse mapping does not belong in the dense per-codepoint table.

**1b. `src/libunicode/bidi.{h,cpp}`** — the UAX#9 algorithm proper. Data-driven throughout: the
W1–W7 / N0–N2 / I1–I2 rules are tables of (class, context) → action, not nested `if`s.

```cpp
namespace unicode {
enum class bidi_direction : uint8_t { left_to_right, right_to_left };

struct bidi_paragraph {
    bidi_direction base_direction;
    std::vector<uint8_t> levels;          ///< resolved embedding level per codepoint (L1 applied)
};

/// P2/P3: first strong character, honouring isolates. Nullopt if the paragraph has none.
[[nodiscard]] std::optional<bidi_direction> bidi_first_strong_direction(std::u32string_view) noexcept;

/// X1-X10, W1-W7, N0-N2, I1-I2, L1: resolve embedding levels for one paragraph.
[[nodiscard]] bidi_paragraph bidi_resolve(std::u32string_view text,
                                          std::optional<bidi_direction> paragraphDirection);

/// L2: reorder one line's level run into visual order. Returns visual index -> logical index.
[[nodiscard]] std::vector<size_t> bidi_reorder_visual(std::span<uint8_t const> levels);
}
```

Plus a `bidi_segmenter` satisfying `consume(out<size_t>, out<Property>)`, so
`run_segmenter.h`'s variadic `basic_run_segmenter<script_segmenter, emoji_segmenter>` becomes
`<script_segmenter, emoji_segmenter, bidi_segmenter>` and the property tuple carries direction for
free. **This is the single change that makes direction flow into Contour's existing run pipeline.**

**Tests — full UAX#9 conformance, and it is not optional.** `_ucd/ucd-17.0.0/BidiTest.txt` (~490k
cases over bidi-class sequences) and `BidiCharacterTest.txt` (~91k cases over real codepoints) are
the official suites and are already on disk. Add `bidi_test.cpp` driving both, plus the existing
`bidi_mirroring_test.cpp`. Anything short of a clean pass on both files means the algorithm is wrong.

Land as a PR against `contour-terminal/libunicode`. Contour consumes it via
`-DCPM_libunicode_SOURCE=D:/libunicode` until it is released; the final commit here bumps
`LIBUNICODE_MINIMAL_VERSION` in `cmake/ContourThirdParties.cmake:61` to the released version.

---

## Phase 2 — vtbackend: paragraph model, modes, logical↔visual mapping

**New `src/vtbackend/Bidi.{h,cpp}`** — the display-only transform, computed per frame, no cache.

```cpp
/// Visual layout of one page's worth of lines, in paragraph context.
struct BidiPageLayout {
    struct LineLayout {
        bidi_direction paragraphDirection;
        std::vector<uint8_t> levels;              ///< per column
        std::vector<ColumnOffset> visualToLogical;
        std::vector<ColumnOffset> logicalToVisual;
        bool identity = true;                     ///< pure-LTR fast path: no permutation at all
    };
    std::vector<LineLayout> lines;
};
```

Built in `Terminal::fillRenderBufferInternal` (`Terminal.h:1810`) *before* `Grid::render`, because
paragraph context reaches outside the viewport: expand the visible line range backwards to the first
line whose `wrapped()` is false and forwards through wrapped continuations, concatenate, run
`unicode::bidi_resolve` once per paragraph, then slice levels back per row.

**No caching, deliberately.** A cache here needs invalidation on every grid mutation, reflow and
scroll — a bug farm for no measured gain. Instead a **cheap fast path**: scan the line for any
codepoint ≥ U+0590; if none, the line is pure LTR, `identity = true`, and nothing is computed. That
covers essentially all real terminal output, and the SoA `codepoints` array
(`LineSoA.h`, 64-byte aligned) makes the scan vectorizable. Measure before adding anything cleverer.

**Modes.** Add to `primitives.h`: `AnsiMode::BiDirectionalSupport = 8` (BDSM) with its
`toAnsiModeNum`/`isValidAnsiMode` rows; `DECMode` entries for `2500` (box mirroring), `2501`
(autodetect) and `1243` (arrow swap) alongside their number-mapping rows. Move `RightToLeftMode`,
`HebrewEncodingMode` and `RightToLeftCopyMode` out of the "settable but not acted on" block at
`primitives.h:911-946` and shrink that comment, as it instructs. Add **SCP** (`CSI Ps SP k`) to the
function table so `0/1/2` set the character path.

Store the resolved state on `Terminal` and expose it to `RenderBufferBuilder`.

**Drive-by fix:** `Terminal.cpp:3005` and `:3272` label `setLeftRightMargin(...)` as `// DECRLM`.
That is DECLRMM/DECSLRM (left-right *margin*), not DECRLM (right-to-left *mode*). Correct both
comments — they will actively mislead once DECRLM means something.

**`RenderBuffer` changes** (`RenderBuffer.h`): add `uint8_t bidiLevel = 0` to `RenderCell`, and
`bidi_direction paragraphDirection` + `bool visuallyReordered` to `RenderLine`.
`RenderBufferBuilder::endLine()` permutes the line's cells' `position.column` through
`logicalToVisual` and sets `groupStart`/`groupEnd` at every **level run boundary** — a shaping group
may never straddle a direction change.

`Grid::render`'s two-path fork (`Grid.h:784-835`) both need handling: `renderTrivialLine` reorders
the `u32string` in place when the line is not `identity` (it is uniform-SGR, so a permuted string is
still one group); `renderCell` goes through the permutation above.

---

## Phase 3 — text_shaper: direction plumbing

Add direction to the shaping API (`shaper.h:141`):

```cpp
virtual void shape(font_key font, std::u32string_view text, gsl::span<unsigned> clusters,
                   unicode::Script script, unicode::PresentationStyle presentation,
                   unicode::bidi_direction direction,      // NEW
                   shape_result& result) = 0;
```

- **`open_shaper.cpp`**: `prepareBuffer` takes the direction and sets
  `HB_DIRECTION_LTR`/`HB_DIRECTION_RTL`. Complete `mapScriptToHarfbuzzScript()` — its
  `// TODO: make this list complete` is now load-bearing, since Arabic and Hebrew must get
  `HB_SCRIPT_ARABIC`/`HB_SCRIPT_HEBREW` explicitly rather than by guess.
- **`directwrite_shaper.cpp`**: pass the real `isRightToLeft` at `:379` and `:440`, set
  `glyphRun.bidiLevel` at `:483`, and make `directwrite_analysis_wrapper.h`'s
  `GetParagraphReadingDirection()` return the actual direction and `SetBidiLevel()` record it.

**The cluster-order trap, and the fix.** `cluster_spans.h`'s `clusterGroups()` returns `nullopt`
when input clusters are not sorted or output clusters are not strictly increasing. HarfBuzz emits
**descending** clusters for RTL runs, so the moment `HB_DIRECTION_RTL` is set, every RTL run degrades
to `indivisibleGroup()` — losing per-cluster font fallback for exactly the scripts that need it most.

Fix by **normalizing at the boundary**: in `shapeWithFont()`, reverse the glyph array for RTL runs
before handing it to `fallbackSegments()`. Everything downstream then sees ascending clusters and
needs no direction awareness at all. This is sound only because of the next change.

**Carry the cluster on `glyph_position`.** Add `unsigned cluster` to
`glyph_position` (`shaper.h:72`). This is the fix the existing TODO at `TextRenderer.cpp:681-685`
already describes ("the datum the pipeline actually has and then drops"), and BiDi makes it a
prerequisite rather than a nicety: once glyphs are placed by *cluster → cell* rather than by an
accumulating pen, visual glyph order stops mattering and the reversal above is free.

---

## Phase 4 — vtrasterizer: RTL run grouping and placement

**`TextClusterGrouper`** (`TextClusterGrouper.cpp`) is the biggest structural change. Today it
flushes a group at every space (`:95-126`), every SGR/color change, and every box-drawing cell — a
group is roughly *one styled word*. Its own comment already names direction as an intended axis
("uniform unicode properties (script, language, direction)").

- Add `bidiLevel` to the flush predicate: a level change ends a group, exactly like a color change.
- **Iterate cells in visual order** within a line, and stop treating the space flush as a hard
  paragraph boundary — a run of Hebrew words must reorder relative to each other, which
  word-at-a-time grouping cannot express. Spaces still end a *shaping* group; they no longer reset
  the direction context.
- `renderLine` (the trivial-line fast path, `:26`) walks codepoints monotonically advancing
  `columnOffset`; it takes the reordered string and per-cell levels from `RenderLine`.

**`TextRenderer`**:
- `createTextShapedGlyphPositions` (`:949`) gets direction from the extended `run_segmenter` tuple
  (Phase 1) and passes it to `shapeTextRun` → `shape()`. The docstring at `:972` that already claims
  "same direction" becomes true.
- `hashTextAndStyle()` (`:178`) must fold **direction** into the shaping-cache key, or an RTL run
  will collide with the identical LTR run in the 4000-entry `strong_lru_hashtable`.
- `renderTextGroup` (`:578`) places each glyph at `initialPen + cluster * cellWidth` instead of
  accumulating `pen.x += advanceToCells(...)`. Within a cluster the pen still accumulates, but the
  *raw* advance — intra-cluster placement is sub-cell, so `advanceToCells` (`GlyphAdvance.h`) has no
  remaining caller and both it and its test were deleted.

**Box-drawing mirroring** (`CSI ? 2500`): `BoxDrawingRenderer` maps U+2500–U+257F to mirrored
counterparts when the cell's level is odd and the mode is set.

**Glyph mirroring**: brackets and other `Bidi_Mirrored` characters in odd-level runs are substituted
via `unicode::bidi_mirroring_glyph()` — already available, no new data needed.

**Arabic presentation-form fallback**: a small `ArabicShaping` helper (joining types from
`ArabicShaping.txt`, via libunicode) mapping a run to U+FE70–U+FEFF, used only when the resolved
font reports no `arab` GSUB script. Keeps Courier New and similar legacy fonts usable.

---

## Phase 5 — cursor, mouse, selection

- **Cursor.** `RenderCursor` (`RenderBuffer.h:89`) gains `bidi_direction direction`. Its `position`
  is mapped through `logicalToVisual`; the renderer draws the bar/underline on the *right* edge for
  odd levels and applies the spec's `⎡`/`⎤` shape hint. Per the recommendation the cursor is hidden
  while `wrapPending` rather than jumping mid-line. `Screen::_cursor.position` stays **logical** —
  nothing in `Screen.cpp` changes.
- **Mouse.** `helper.cpp:178-181` converts pixels to a column with plain `(sx - marginLeft) /
  cellWidth`. Route the result through `visualToLogical` before it reaches `Terminal`, so both mouse
  *reporting* to the app and selection anchors are logical — the spec's inverse transform.
- **Selection.** `Selection` (`Selector.h`) keeps logical `CellLocation` anchors, so
  `extractSelectionText()` yields logical order and copy/paste is correct by construction. Two things
  must change: `SelectionHelper` gains visual-order awareness for hit-testing, and
  `SelectionRenderer` (`Terminal.cpp:2168`) must stop inferring line breaks from
  `pos.column < lastColumn` — pass the line explicitly. A logically contiguous selection rendering as
  visually discontiguous is **correct** and expected.
- **Arrow keys** (`CSI ? 1243`, default on): in `sendKeyEvent`, swap `Key::LeftArrow`/`RightArrow`
  when the cursor sits in an RTL paragraph.
- **IME.** Preedit already exists (`TerminalDisplay::inputMethodEvent`, `:1263-1277`;
  `RenderBufferBuilder::tryRenderInputMethodEditor`, `:631-662`). Render the preedit string with the
  paragraph direction, and fill in `ImCurrentSelection` in `inputMethodQuery` (`:1280-1327`), which
  currently always returns empty.

---

## Phase 6 — documentation and release notes

- `docs/internals/text-stack.md:175-178` — replace "Bidirectional text was not addressed" with the
  actual design.
- `docs/internals/vt-conformance.md:130-139` — remove the "toggle is a no-op" entry; the modes now
  act.
- Add the reference trees to the `vt-conformance.md:210-220` table.
- New user-facing `docs/rtl-bidi.md`: the five escape sequences, defaults, and font recommendations.
- Release note via `/add-release-note`.

---

## Verification

**Build against the local libunicode branch:**

```
cmake --preset clang-debug -DCPM_libunicode_SOURCE=D:/libunicode
cmake --build --preset clang-debug
```

**Tests, in dependency order:**

1. `D:\libunicode` — `bidi_test` must pass **all** of `BidiTest.txt` and `BidiCharacterTest.txt`
   cleanly. This is the gate for everything downstream.
2. `vtbackend_test` — new `Bidi_test.cpp` (paragraph assembly across `Wrapped` lines, level
   resolution, the `identity` fast path, logical↔visual round-trip); `Screen_test.cpp` extensions for
   BDSM/SCP/2500/2501/1243 and the now-behavioural DECRLM/DECHEM/DECRLCM.
3. `text_shaper_test` — direction plumbing against `mock_font_locator`; RTL cluster normalization in
   `cluster_spans_test.cpp` (a descending-cluster run must now segment, not degrade); a real-font
   Arabic joining test gated on Noto Naskh Arabic being installed.
4. `vtrasterizer_test` — `TextClusterGrouper_test.cpp` cases for level-run boundaries and visual
   iteration; `TextRenderer_test.cpp` for cluster-based placement.
5. `contour_gui_test` — an offscreen `DisplayRendering_test` case rendering a mixed
   Hebrew/Arabic/Latin/digit screen.
6. `ctest --preset=clang-asan` clean, and `--preset=clang-coverage` for the coverage figure.

**End-to-end, by eye** — this is what the fonts are for. Run Contour and `printf` a fixture:

- Pure Hebrew (`שלום עולם`) — right-aligned, letters right-to-left.
- Pure Arabic (`العربية`) — letters *joined*, not isolated. This is the shaping test.
- Mixed LTR/RTL with digits (`abc שלום 123 def`) — digits stay LTR inside the RTL run (rules W2/W7).
- Mirrored brackets: `(שלום)` must render with the parens visually swapped.
- Wrapped paragraph longer than the terminal width — reordering must respect the whole paragraph,
  not each row.
- Arrow-key navigation and click-to-position through mixed text; select across a direction boundary
  and confirm the clipboard holds **logical** order.

**Cross-check against the references** in `D:\others`: `vte/src/bidi.cc` for the paragraph/row model
and `mlterm/vtemu/vt_bidi.c` plus `vt_shape.c` for Arabic presentation-form handling.

**Performance.** BiDi runs per frame over the visible page. The ≥ U+0590 scan must keep ASCII output
at parity — verify with `valgrind --tool=callgrind` on a `cat` of a large ASCII file, comparing
against master. Report the delta in the change summary alongside risk and coverage, per AGENT.md.

## Risks

- **Highest: `TextClusterGrouper` reshaping.** It is on the hot path for *all* text, LTR included.
  The space-flush change touches every render. Mitigated by keeping the `identity` fast path
  byte-identical to today's behaviour and by the existing grouper tests.
- **Shaping-cache key.** Forgetting direction in `hashTextAndStyle` produces wrong glyphs
  intermittently and only under mixed content — the kind of bug that passes every test. Add a
  targeted cache-collision test.
- **UAX#9 is large.** Mitigated entirely by the two official conformance files being on disk; there
  is no guessing involved.
- **Cross-repo sequencing.** Contour cannot merge until libunicode releases. The `CPM_libunicode_SOURCE`
  override keeps development unblocked; the final commit bumps the version.
