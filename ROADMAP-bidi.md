# BiDi — what is done, what is not

Working notes for the `feature/rtl-text` branch. Branch-scoped: nothing in `src/` may reference this
file. Durable knowledge belongs in `docs/internals/` and `docs/rtl-bidi.md`.

Companion: `PLAN-bidi.md` (the original plan).

## State

Contour `feature/rtl-text`, libunicode `feature/bidi`. `ctest --preset=clang-asan` is 16/16 green,
but that means *nothing regressed* — the tests that would exercise the unfinished parts are
themselves unfinished. Do not read the pass rate as coverage.

### Works end to end today

Hebrew and Arabic reorder correctly per paragraph, across soft-wrapped lines. Arabic joins. Brackets
mirror. Digits inside RTL runs behave (W2/W7). All control sequences work: BDSM, SCP, DEC modes 2500
(stored only), 2501, 1243, and real behaviour for DECRLM/DECHEM/DECRLCM. Cursor position and mouse
clicks map between visual and logical space. Arrow keys swap in RTL runs.

## Done

- **Phase 0** — reference trees (`vte`, `mlterm`, `fribidi`), six fonts, docs.
- **Phase 1** — libunicode: `Bidi_Class` + `Bidi_Paired_Bracket_Type` (verified against the UCD for
  all 1,114,112 codepoints), UAX#9 (`BidiTest` 770241/770241, `BidiCharacterTest` 91707/91707),
  `bidi_segmenter` folded into `run_segmenter`, `_GLIBCXX_DEBUG` ABI leak fixed.
  - *Deviation:* the plan's generated `bidi_paired_bracket()` was dropped as unnecessary —
    `bidi_mirroring_glyph()` already yields a bracket's expected closer, and the new
    `bidi_paired_bracket_type` field supplies what mirroring cannot.
- **Phase 2** — `Bidi.{h,cpp}` paragraph model, identity fast path, all modes, render-buffer
  permutation across all three render paths.
- **Phase 6** — `docs/rtl-bidi.md`, `text-stack.md`, `vt-conformance.md`, mkdocs nav, release note,
  spell-gate terms.

## Not done

Ordered by user-visible impact.

### 1. Cluster-based glyph placement — DONE
`renderTextGroup` places each glyph at `initialPen + cluster * cellWidth`. The accumulating pen is
gone, and with it `advanceToCells` and its `GlyphAdvance.h` — the rounding heuristic existed only to
guess a cell count from a font advance, which is now known exactly.

The precondition was cleared first, as the order of work demanded. `TextClusterGrouper` counts grid
**columns** rather than cells appended, taking the span from `GlyphSizing::columns` — the field whose
documented job is already "how many cells the block spans, as the backend claimed them", so the number
gains no third spelling.

The earlier note that a wide cell's continuation blank already flushes the group turned out to hold on
every current call path — but by coincidence of three independent producers, not by construction.
Driving `renderCell` directly with two adjacent wide cells yields clusters `{0, 1}`; both new grouper
tests fail on the old code (`{0,1}` vs `{0,2}`, and a following group starting at column 2 vs 3).

Advances still rule **within** a cluster: a base and its combining marks are positioned against each
other by the shaper, not against the cell grid, so the pen accumulates until the cluster changes — and
it accumulates the *raw* advance, since intra-cluster placement is sub-cell. That is also why the
right-to-left glyph reversal stays. Placement no longer depends on draw order, but intra-cluster
accumulation is only valid in HarfBuzz's own output order, which for an RTL run is visual.

Two producers had to be corrected before the column span was honest:
- `RenderBufferBuilder::makeRenderCellExplicit` set `width` but left `sizing.columns` at 1, so a wide
  grapheme cluster in a status line or IME preedit string counted as one column.
- `TextClusterGrouper::renderLine` (the trivial-line path) passed a default sizing for every cell.

And one hazard the change created, now closed: `directwrite_shaper` never populated
`glyph_position::cluster`. That cost nothing while the field was advisory; with placement reading it,
every glyph of a run would have been drawn stacked on the group's first cell. Both its simple and
complex paths now set it — the complex one by inverting DirectWrite's cluster map, which runs the
opposite way to HarfBuzz's (text position → first glyph). Still unverifiable here; see item 8.

**Two design points deferred, both raised by review and both larger than this change.**

*`RenderCell::width` and `sizing.columns` are one datum with two names.* Every producer writes both;
the consumers are split (`BackgroundRenderer` and `DecorationRenderer` read `width`, `TextRenderer`
reads `sizing.columns`). An audit found no producer today that sets one without the other, but nothing
enforces it. The right fix is to delete `RenderCell::width` and point its two readers at
`sizing.columns` — a `vtbackend` change touching two renderers, so not folded in here. Related:
`GlyphSizing::columns` is documented as `scale * width` while all three producers store plain `width`,
and `TextRenderer` then divides by the block scale. That contradiction predates this branch but is now
load-bearing for every glyph.

*The cluster could be derived from `position.column - initialPen.column` instead of being plumbed.*
The grouper is already handed each cell's position, so on the face of it the span is redundant. It is
not, yet: positions are **display** columns — `RenderBufferBuilder` multiplies by two on a DECDWL line
(`displayColumn = column * 2`) while `advanceScale` doubles again at draw time — and `renderLine`
applies that scale to a wide cell's continuation columns but not to the head. Deriving the cluster
from a position means untangling that first. Worth doing; it would delete the plumbing and fix the
DECDWL inconsistency at the same time.

### 2. Selection — RESOLVED, the plan was wrong here
Now covered by tests (`Bidi.selection yields logical order`, `Bidi.selection across a direction
boundary is logical`) and no code change was needed.

The plan called for changing `SelectionRenderer`'s
`pos.column < lastColumn || (pos.column == lastColumn && lineStarted)` line-break inference, on the
grounds that reordering breaks it. It does not: that callback is fed **logical** grid positions, and
under the display-only model Selection never sees the permutation at all. The columns are still
monotonic, so the predicate still holds.

`SelectionHelper` likewise needs no visual-order hit-testing, because mouse pixels are already mapped
visual to logical in `helper.cpp` before they ever reach `Terminal` -- so the anchors that arrive are
logical by the time selection sees them.

Note a logically contiguous selection may render visually **discontiguous**. That is correct and
matches VTE; do not "fix" it.

### 3. Cursor — DONE
`RenderCursor::direction` is now consumed: `CursorRenderer::render()` shifts a Bar cursor to the
cell's trailing edge in a right-to-left run, with the thickness expression extracted so the tile and
the placement cannot drift apart. Underscore spans the full width and Block/Rectangle fill it, so
neither needs anything.

The `⎡`/`⎤` shape hint is now DONE, and the plan's wording for it was misleading: those characters
name an *appearance*, not glyphs to draw. VTE fills a `stem_width` square at the top of the I-beam, on
the side the text flows toward — so the bar reads as `|-` going left-to-right and `-|` going
right-to-left. `CursorRenderer` does the same, from a direct-mapped tile of its own.

Gated on a new `BidiLineLayout::mixedDirection`, which is VTE's `has_foreign`: does any character
resolve to a level other than the paragraph's own. Three things about it are load-bearing:
- It is a **paragraph** property, stamped on every row, so the cursor does not change shape as it
  crosses a soft wrap inside one paragraph.
- A paragraph that runs one way throughout gets **no** hint — including a pure-Hebrew one, where base
  and characters agree and nothing is in doubt. Without this gate every cursor on every ASCII line
  would carry the extra square.
- Latin in a *forced* right-to-left paragraph does count as mixed: the Latin resolves above the base
  level and is genuinely the foreign direction there.

Bar only, as in VTE, whose own note says the other shapes want a visual design nobody has settled on.
A block or rectangle has no stem to hang the square off; an underscore already spans the cell.
VTE additionally guards on focus — Contour gets that for free, because an unfocused cursor is already
substituted to `Rectangle`, which the hint does not apply to.

### 3b. Hiding the cursor while `wrapPending` — RECOMMENDED AGAINST
The remaining half of the plan's cursor item should NOT be implemented as written.

It is not a bidi behaviour at all, and not scoped to RTL: it would hide the cursor for every user who
types into the last column of any line. It also does not describe something VTE *does* — it describes
a consequence of a cursor model Contour does not use. Per the recommendation itself, VTE puts the
cursor in column 81, off-screen, instead of xterm's "stay in column 80 and set a flag"; the cursor is
not hidden, it is simply outside the visible area. Contour uses xterm's model, and reports the flag in
DECCIR (`Screen.cpp`, `sflagBits |= 8`).

Checked against the reference trees: none of foot, kitty, ghostty, mintty or konsole suppresses the
cursor on a pending wrap. ghostty tracks `pending_wrap` purely for print/wrap logic. VTE is alone, and
only incidentally.

Adopting it would mean emulating a side effect of someone else's cursor model, diverging from every
other terminal, for a visible regression in ordinary LTR use. If the underlying complaint — the cursor
appearing to jump mid-line in an RTL paragraph — turns out to bite in practice, the fix belongs in
where the cursor is *placed*, not in hiding it.

### 4. `TextClusterGrouper` space flush — NOT NEEDED, the plan was wrong here too
Same root cause as the selection item: the plan assumed the grouper does the reordering. It does not.
`RenderBufferBuilder` permutes the cells first, so by the time the grouper sees them the words are
already in visual order and a space may go on ending a shaping group.

Covered by `Bidi.multiple RTL words reorder relative to each other`. The hot path is untouched, which
is the best outcome available for what the plan called its highest-risk change.

### 5. Box-drawing mirroring (`CSI ? 2500`) — DONE
And the earlier note that it "needs a judgement per glyph" was wrong. `detail::Box` already models a
glyph structurally (up/right/down/left, arcs, diagonal), so the mirror is a reflection of that
struct: left and right swap, the arc pairs swap, a diagonal reverses. No codepoint table, and it
holds for every glyph rather than the ones someone remembered.

Tests compare bitmaps rather than trusting the derivation: mirrored U+250C must equal U+2510 exactly,
and glyphs already symmetric about the vertical axis must come back unchanged.

The mode rides on `RenderBuffer` as per-frame terminal state, and the mirrored tile takes bit 30 of
the box-drawing atlas cache key -- it is a different bitmap, so it needs its own entry.

### 6. Arabic presentation-form fallback
Absent. Maps a run onto U+FE70..U+FEFF when the resolved font has no `arab` GSUB. No font on the
tested platforms lacks it, so the branch cannot be exercised locally.

### 7. IME
Preedit is not rendered with the paragraph direction, and `ImCurrentSelection` in `inputMethodQuery`
still always returns empty.

### 8. DirectWrite is UNVERIFIED
Written blind — it cannot be compiled on Linux. Must be checked on Windows before release.

Check the clusters first. `glyph_position::cluster` is now load-bearing: the renderer places a glyph at
the column its cluster names, so a run coming back with all-zero clusters draws stacked on its first
cell. Obvious on screen, invisible to every test in this tree. Reachable via `text_shaping.engine` set
to `native` or `dwrite`.

Unrelated and pre-existing, but adjacent: the complex-shaping path never sets `gpos.presentation`,
while the simple path does. That feeds the atlas cache key.

## Missing tests and verification

- ~~Cache-collision test for `hashTextAndStyle`~~ — DONE. `hashTextAndStyle` moved to its own header
  (`TextShapingCacheKey.h`) so it is testable; three cases, verified against a deliberately
  direction-blind key which fails two of them. Covers the zero-enumerator trap from both sides
  (`Bidi_Direction::Left_To_Right` and `TextStyle::Invalid` are both 0, and this only works because
  `operator*(strong_hash, uint32_t)` mixes rather than multiplies).
- ~~RTL cluster normalization in `cluster_spans_test.cpp`~~ — DONE, both halves of the contract.
- ~~`TextClusterGrouper_test`: level-run boundary cases~~ — DONE, and it caught a real bug: the
  level term was never in the flush predicate at all, despite commit 177803fc claiming it was.
- ~~`TextRenderer_test`: cluster-based placement~~ — DONE, as sections of the issue-#1939 case, which
  asks the same question ("does a run stay on the cell grid?") for one-column cells. The discriminating
  one gives a fallback font's ideograph a 9px advance against two 10px cells: the accumulating pen
  rounds that to one cell and draws the next glyph at 10, cluster placement puts it at 20. Plus a
  right-to-left guard — a regression guard only, since a one-column cell cannot round to anything but
  one cell, but it does catch either of the two RTL reversals being dropped.
- Offscreen `DisplayRendering_test`: a mixed Hebrew/Arabic/Latin/digit screen.
- A real-font Arabic joining test, gated on FreeMono or Noto Naskh Arabic being present.
- ~~Performance comparison against the merge-base~~ — DONE, and it found a real regression.
  Rendering a 40x120 all-ASCII page was **+29.6%** (7.53 → 9.76 us/frame); now **+6.1%**
  (7.35 → 7.79 us/frame, ~0.45us/frame, about 0.003% of a 60Hz budget). Method: hidden `[.perf]`
  case in `Bidi_test`, release build, interleaved against a detached worktree at the merge-base,
  min of 8. `bench-headless` cannot answer this — it drives the parser and never reaches
  `fillRenderBufferInternal`. **Interleave and take minimums**: sequential runs drifted 2x under
  varying machine load and gave a nonsense answer first time.

## Environment notes

- `_deps/sources/CMakeLists.txt` (git-ignored) points at `~/projects/libunicode`; it pinned 0.9.1 and
  failed the 0.9.2 gate from commit `62212ab5`. Backup in the session scratchpad.
- Contour cannot merge until libunicode releases; bump `LIBUNICODE_MINIMAL_VERSION` then.
- A concurrent Claude session may run in a sibling worktree — never use a broad `pkill` pattern.
- In tests use `refreshRenderBuffer()`; `ensureFreshRenderBuffer()` leaves the buffer empty under
  MockTerm. Write Hebrew test data as `ש`-style escapes — RTL literals cannot be eyeballed.
