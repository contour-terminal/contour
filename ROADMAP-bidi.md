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

### 1. Cluster-based glyph placement — BLOCKED, not merely undone
`renderTextGroup` still advances an accumulating pen (`pen.x += advanceToCells(...)`). The plan calls
for `initialPen + cluster * cellWidth`, and `glyph_position::cluster` now exists and is populated, so
the datum is there.

**The precondition is not met.** The TODO at `TextRenderer.cpp` (just above the `pen.x +=` line) says
so explicitly, and `TextClusterGrouper.h:117` carries the matching
`int _cellCount = 0; // FIXME: EA width vs actual cells`. Clusters count **cells appended**, not
**columns occupied**, so for a double-width glyph the cluster index advances by one while the glyph
covers two columns. Switching to `cluster * cellWidth` without fixing that would misplace every glyph
after a CJK or emoji character — a far worse regression than the rounding it removes.

Order of work: fix the grouper to count columns (its own FIXME, which predates this branch), *then*
switch the placement. Do not do the second without the first.

Note that in practice a wide cell's continuation column arrives as a blank and flushes the group, so
a group may already be one-column-per-cluster — but that is an inference about current call paths,
not an invariant, and the FIXME is the authority.

### 2. Selection in mixed text — `Selector.*`, `Terminal.cpp:2286`
Untouched. Two pieces:
- `SelectionHelper` needs visual-order hit-testing.
- `SelectionRenderer` infers a line break from `pos.column < lastColumn || (pos.column == lastColumn
  && lineStarted)`. That is false under reordering; pass the line explicitly.

A logically contiguous selection rendering as visually discontiguous is **correct** — do not "fix"
that.

### 3. `RenderCursor::direction` is a dead field
Added and populated, consumed by nobody. The bar/underline cursor still draws on the left edge of its
cell inside an RTL run; it should draw on the right, and apply the recommendation's `⎡`/`⎤` shape
hint. Also: the cursor should be hidden while `wrapPending` rather than jumping mid-line.

### 4. `TextClusterGrouper` space flush
`appendCellTextToClusterGroup` is unchanged from master. The plan wants a space to stop being a hard
group boundary so that a run of RTL words can reorder relative to one another; spaces should still
end a *shaping* group without resetting the direction context. **Highest-risk item in the plan** —
it is on the hot path for all text, LTR included.

### 5. Box-drawing mirroring (`CSI ? 2500`)
Mode is stored and reported; nothing reads it. Needs a U+2500..U+257F mirror table, which is a
judgement per glyph rather than a derivation. Defaults to reset, so default rendering is unaffected.

### 6. Arabic presentation-form fallback
Absent. Maps a run onto U+FE70..U+FEFF when the resolved font has no `arab` GSUB. No font on the
tested platforms lacks it, so the branch cannot be exercised locally.

### 7. IME
Preedit is not rendered with the paragraph direction, and `ImCurrentSelection` in `inputMethodQuery`
still always returns empty.

### 8. DirectWrite is UNVERIFIED
Written blind — it cannot be compiled on Linux. Must be checked on Windows before release.

## Missing tests and verification

- **Cache-collision test for `hashTextAndStyle`** — the plan singles this out as "the kind of bug
  that passes every test". Direction is folded into the key; nothing proves it.
- RTL cluster normalization in `cluster_spans_test.cpp`: a descending-cluster run must segment, not
  degrade to `indivisibleGroup()`.
- `TextClusterGrouper_test`: level-run boundary cases.
- `TextRenderer_test`: cluster-based placement.
- Offscreen `DisplayRendering_test`: a mixed Hebrew/Arabic/Latin/digit screen.
- A real-font Arabic joining test, gated on FreeMono or Noto Naskh Arabic being present.
- **Callgrind comparison against master** on a `cat` of a large ASCII file. AGENT.md requires a perf
  figure for hot-path changes, and the grouper is on the hot path for all text.

## Environment notes

- `_deps/sources/CMakeLists.txt` (git-ignored) points at `~/projects/libunicode`; it pinned 0.9.1 and
  failed the 0.9.2 gate from commit `62212ab5`. Backup in the session scratchpad.
- Contour cannot merge until libunicode releases; bump `LIBUNICODE_MINIMAL_VERSION` then.
- A concurrent Claude session may run in a sibling worktree — never use a broad `pkill` pattern.
- In tests use `refreshRenderBuffer()`; `ensureFreshRenderBuffer()` leaves the buffer empty under
  MockTerm. Write Hebrew test data as `ש`-style escapes — RTL literals cannot be eyeballed.
