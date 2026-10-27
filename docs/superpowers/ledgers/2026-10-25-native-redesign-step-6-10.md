# SDD ledger — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-6-10.md

Spec: §3.16 (row 6.10) of docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md. Written ahead (pipelining); pre-flight before execution.

## Owner decisions and gaps (ruled by the controller under the owner's goal)
- Ruling D1: Classic keeps today's highlighted preedit; Native2026 gets the inline 1 pt underline; a preedit too long for the line is cut at the right edge — cost if wrong: an edition default.
- Spec gaps ruled: the end-of-line cursor and a moving pane get a cursor rectangle (today they get none, sending the candidate window to the pane's origin); the `input_method_editor` scheme colours stay for Classic; the preedit is not chrome, so it is tested, not snapshotted.

## Pre-flight
preflight.md (opus): 2 blocking / 12 non-blocking / 2 owner questions.
## Rulings
- P1: all findings accepted — notably Decorator's slot count derived from its last enumerator (not numeric_limits pinned to Encircle), so PreeditUnderline gets its own tile slot; the IME display test runs in a step's one targeted display pass (or a renderer-backed offscreen harness), never vacuously.
- Q1–Q2 as proposed: (1) the inline preedit follows the screen (reversed under DECSCNM); (2) the new decorator is internal only, not a configuration value.
- Rule (owner): offscreen per task; one targeted display pass in Finish; self-check list in every brief; small fix rounds controller-checked; small related tasks batch three.
- 2026-10-27 12:05:07 OWNER (pace): one on-screen (display) test pass per phase instead of per step. Phase 3's pass runs at 3f's Finish and covers 3e's and 3f's rows (3e already ran its own). Phase 4's pass runs at 4b's Finish; Phase 5's at the later of 5a/5b; Phase 6's at the last 6.x. Step Finishes otherwise run offscreen only.
- 2026-10-27 12:17:34 OWNER (order): Phase 6 starts as soon as a lane frees, ahead of Step 5. Ruled order by readiness: 6.10 IME (no step deps), 6.6 secure input, 6.8 keyboard to chrome, 6.2 restore, 6.3 exited process, 6.4 drag and drop, 6.9 bell/dock (its Linux-launcher part waits for 5b, its Windows part for 5a), 6.1 shell integration card (needs 4b's settings row); then 5a/5b; then 6.5 finished elsewhere (needs 3f, 5a, 5b) and 6.7 quick terminal (needs 4b, 5b). Each moment's pre-flight references are re-verified by its implementer against the current tip before Task 1. Moments keep landing on feature/native-redesign-phase0 like every step so far; splitting into one PR each is the owner's call at merge time. Cost if wrong: a moment whose deferred task (Linux/Windows part) is forgotten — each is listed in that moment's ledger.
- 2026-10-27 12:32:43 Start: fourth lane (owner freed disk: 263 GB free) — worktree /Users/christianparpart/projects/contour-lane4, branch work/native-redesign-step-6-10 from 8ffd00cb0. Tasks 1–3 dispatched (implementer); references re-verified against the tip first.
- 2026-10-27 14:54:10 Tasks 1–3: e81d423ad (candidate window at the cursor cell, end of line included), 135221578 (inline preedit in the cell's colours, cut at the edge; input_method_editor_style), f41103e62 (1-pt underline). vtbackend 3006, vtrasterizer 167, [contour][ime] 17, [config] 312, daemon 6/6, vtconformance.
- Rulings: the found bug (no preedit on a plain row, both styles) fixed in Task 2 and the CursorVisibleDuringComposition expectation corrected — accepted. F1 superseded by the renderer now existing at setSession (tested offscreen against the real grid) — accepted. The block cursor painting the preedit's first cell goes on Phase 6's display-pass list. input_method_editor_style's Settings row → 4b carry-in. Other deviations accepted.
- Review dispatched (task-reviewer, opus).
- 2026-10-27 15:00:33 Review of 1–3 (task-1-3-review.md): I1 a wide first cluster at the last column draws no preedit; I2 the decorator slot count unprotected. Rulings: I1 — the preedit shifts left so its end meets the right edge when its first cluster does not fit (the composition must stay visible; D1's cut applies only to a preedit longer than the line), and the candidate window follows the shifted start; I2 static_assert as suggested; M4 — '1 pt' is one logical point (macOS/Qt device-independent pixel): 1 px at 1×, 2 px at 2×, rounded at 1.5× — not 96-DPI typographic points; M7 includes clearing on setSession and on gutter/margin change, and pageMargin.left includes 3f's rail after the rebase. All minors fixed in the same round; Task 4 (Finish) follows.
- 2026-10-27 Fix round (task-1-3-review.md), red first, in the lane4 tree:
  - I1: `vtbackend::placePreedit` (screen/PreeditPlacement.{hpp,cpp}): from the cursor when the preedit fits; otherwise it moves left until its end meets the right edge; only a preedit wider than the whole line is cut. RenderBufferBuilder triggers at the placement's start on both row paths (renderCell and the plain-row overlay); the candidate window anchors at the same start through `imePreeditAnchor` (ImeQueryRect.hpp), as wide as the first cluster. Reading the ruling: "D1's cut applies only to a preedit longer than the line" means a preedit that fits the line but runs past the edge from the cursor also moves left, not only one whose first cluster does not fit.
  - I2: static_asserts in Decorator.hpp (the names tables' sizes == count; every index named once and < count). The slot test now walks the names tables instead of eachElement, and fails without the guard.
  - M1: comments fixed. M2: a zero-width cluster takes one column (clamped as renderGridText does), and the overlay counts the moved cells against the columns. M3: the placement cases are generated over a uniform row and a mixed-rendition row × both styles.
  - M4: `logicalPointsToPixels` = max(1, round-half-up(dpi / 96)): 96→1, 144→2, 192→2, 288→3.
  - M5: straight and dashed strokes are clamped to the room below the underline position. The family test runs at (9,1), (9,3) and (3,4). Position 2 is left out because the curly wave grows its tile to three rows there by design (curlyUnderlineGeometry).
  - M6: `RenderOnlyCellFlags` is masked on both wire decode sites, with a test.
  - M7: the last anchor *cell* is stored instead of its rectangle, so margin, gutter, font and DPR changes are followed by construction; it is cleared on setSession(). 3f's rail is not in this tree yet: after the rebase, pageMargin.left carries it, so the cell rectangle includes it.
  - M8: re-announcement tests record the rectangle answered at each announcement. The font case that checks the new cell's rectangle is `[display][ime]` (offscreen only stages the font).
- Task 4 (Finish): /simplify done by hand (no subagents in this lane); no display pass (Phase 6's single pass runs at the last moment); the Settings row for input_method_editor_style is Step 4b's carry-in. Callgrind not run: valgrind is not available on macOS; Linux CI or the phase pass should measure bench-headless.

## Phase 6 display-pass list (from 6.10)
- `contour_gui_test "TerminalDisplay.ime.aFontChangeAnnouncesTheNewCellsRectangle"` ([display][ime]).
- Block cursor over the preedit: with a block cursor, the preedit's first cell is painted in cursor colours (makeColorsForCell keys the cursor on the grid position) while the render cursor stands after the preedit. Check under both styles, and decide.
- Retina underline: the inline preedit's underline is 2 device px at 2× and 1 px at 1× (one logical point), at the font's underline position.

## Owner manual checks (6.10, batched for Phase 6, not awaited)
- On `contour.app` with a fresh Native2026 config and macOS Japanese – Romaji input, type `nihongo` at the prompt. `にほんご` appears inline in the foreground colour with a thin underline, and Space opens the candidates below the first preedit cell.
- Fill the line to the last column, then compose. The preedit moves left to end at the edge, and the candidates sit under its first cell.
- Compose in the right pane of a split, then drag the divider: the candidates follow. Press ⌘+ while composing: they follow too.
- With `defaults_edition: classic`, the preedit is bold, underlined, white on red, as before.
