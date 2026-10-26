# SDD ledger — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-3d.md

## Pre-flight
Full scan: preflight.md in this workspace (opus), against the Step 2 branch. 21 findings: F1, F2, F4, F5 blocking; 17 non-blocking. §3.10 fully covered.

## Rulings
- Ruling P1: all findings accepted with the scan's proposed resolutions (F1 assign the whole Config::profiles map in tests and sort children by argument; F4/F5 BarText → BarInk and the two enum-end counting tests updated; F8 closePolicy CloseOnPressOutside with Escape handled in the field so Escape steps back a level) — cost if wrong: local rework.
- Ruling Q1: window margin s7 (16). Q2: focus hands back for any Overlay item, with a find-bar test. Q3: the catalogue's splitCamelCase casing everywhere ("Attach To Host ›") — one source of titles beats the board's copy — cost if wrong: one table of titles. Q4: the palette is a real popover, so the Terminal row keeps its hard one-cell e3 shadow via ElevationShadow.
- Order: after 3c.
- 2026-10-26 17:25:05 Rule (owner): offscreen only per task and fix round; one targeted display pass in Finish.
- 2026-10-26 18:09:26 OWNER (pace): fix rounds under ~150 changed lines are controller-checked (re-review agents only for threading, macOS/AppKit or Critical); small related tasks batch three per implementer; every brief carries .superpowers/sdd/brief-self-check.md; a third worktree lane once the owner frees ~25 GB.
- 2026-10-26 19:36:38 Tasks 1–3 dispatched (opus) in the main tree at cdd213ff (parallel with 3b in lane3).
- Tasks 1–3: implementer DONE — e196eefb (a palette category per catalogue action, one table; 117 rows; SearchSelection waits for 3b — whichever rebases second adds { SearchSelection, Edit }), fe6620f2 (grouped palette, drill-in rows, ranking across children), ebf9650a (the model publishes the grouped listing and drills in). Filters pass; [snapshot] 5 + 1 skipped; lint. Brief deviation: splitv matches 'Split Vertical', so the id-only fallback test uses clipboard:strip; two brief cases strengthened (they could not fail). Main tree rebased onto d8bdfa7d (the pushed main line). One review dispatched (opus).
- Tasks 1–3: review (opus) — Spec ✅, Approved. The CommandPaletteTree_test :227 'dangling' worry is unfounded (ranges::contains returns bool). Category coverage guarded (static_assert + per-row test); 3b's SearchSelection hand-off forced by the compiler. Minors folded into the next batch's first commit: restore the two dropped CommandPaletteModel_test assertions (clearing the filter clears the highlight; one of the two Splits under the cursor for "spl"); drop the vacuous headers.size() == count check; refresh() emits levelChanged when _anchorRow changes. Minor 4 (duplicated scoring) → Finish/simplify; Minor 5 (byte-wise fuzzy matching across UTF-8 boundaries) → follow-up issue; Minor 6 accepted. Do NOT push the main tree before Task 5 (unfiltered browsing cannot reach drill-in children until then).
- Tasks 1–3: complete (94a97c50, dd525a85, bb9faf62 after the rebase). Tasks 4–6 dispatched (resume the implementer), BASE bb9faf62.
- 2026-10-26 21:25:40 Session restarted (account change); the lane agent was resumed by message. From here: implementer / task-reviewer / code-reviewer agent types per ~/.claude/CLAUDE.md.
- 2026-10-26 22:06:53 Tasks 4–6 committed: 775f1df43, 5925fc1f4, 3870be8b8 (on 92d4ffde8). Task review dispatched (task-reviewer, opus).
- 2026-10-26 22:06:53 Ruling: the palette contrast loop (rows × appearances, every ink ≥ 4.5:1, marks ≥ 3:1) is added now, not deferred — self-check item 3 requires it; the OS selection pair stays untestable and is commented — if wrong: one test that fails in CI on a scheme we never looked at.
- 2026-10-26 22:06:53 Task 7 dispatched to the same implementer in parallel with the review; Task 8 (Finish) is the controller's.
- 2026-10-26 22:14:42 Task review (Tasks 4–6): Spec ✅ / Changes requested — 1C (selected row invisible where the selection falls back to the bar), 3I (keys lost during the fade-in; native contrast unasserted; RTL check vacuous), 7M. Review: task-4-6-review.md.
- 2026-10-26 22:14:42 Ruling: fix round takes C1, I2, I3, I4, M5, M6, M7, M9, M10, M11; M8 goes to the Finish display pass. Agent re-review (Critical).
- 2026-10-26 22:14:42 Owner follow-up: R-h exempts the OS highlight pair citing macOS only; on ContourLinux Breeze's default #3DAEE9 + white is ~2.5:1 — the owner decides whether Linux keeps the OS pair.
- 2026-10-26 22:47:29 Fix round committed: dd86aa723 (C1: BarSelection/BarSelectionText scheme slots with a fallback ending in inverse video on the bar; I3 contrast loop every scheme × row × appearance — failed 8 checks before), 0c56d5a0f (I2, I4, M5, M6, M7, M9, M10, M11). Task 7 committed: 20c2100e6. Re-review of the round + Task 7 review dispatched together (task-reviewer, opus).
- 2026-10-26 22:47:29 Ruling (concern 1): on the four schemes where the palette's selected row falls back to inverse video, tabs keep their own selection (they are also marked by weight) — the palette needs a fill that reads, tabs already have one — if wrong: one more consumer of the new slots.
- 2026-10-26 22:52:34 Re-review (A) all addressed; Task 7 (B) Spec ✅ / Approved. Five Minors fold into Finish: Escape before opened; M6 falls back to the nearest visible overlay ancestor (with a test that can fail); contrast loop exemption by colour source + count tie + separator; scene comment; PaletteDrilledIn asserts opened.
- 2026-10-26 22:52:34 Task 4: complete (fix round 1)
- 2026-10-26 22:52:34 Task 5: complete (fix round 1)
- 2026-10-26 22:52:34 Task 6: complete (fix round 1)
- 2026-10-26 22:52:34 Task 7: complete
- 2026-10-26 22:52:34 Task 8 (Finish) Steps 1–5 dispatched to the implementer; Step 6 is the controller's after the final review.
- 2026-10-26 22:52:34 Owner follow-up: on one-dark the Terminal row's active tab has no fill either (bold only); moving tabs onto BarSelection is a separate decision.

## Owner manual checks (Step 3d, batched; not awaited)
On this Mac, `out/appleclang-debug/src/contour/contour.app`:
- ⇧⌘P (Ctrl+Shift+P): the palette opens with a short scale and fade; with Reduce Motion on, a fade only.
- The rows group under Recently used, Pane, Tab, …; ⌃J/⌃K and the arrows move; the footer shows the selected row's description and "↩ Run · ⎋ Close".
- "Change Profile ›" → Enter lists the profiles; typing narrows them; Escape goes back with the old query; Escape closes. Typing a profile's name at the top runs it directly.
- Attach To Host › lists the saved hosts with their URL in the footer.
- Open About from the menu bar, then the palette from the View menu (or the menu bar's palette row), Escape: the About dialog still has the keyboard.
- VoiceOver reads the field as "Command", a header as a heading, and a drill-in row's "Opens a list".
- `ui_style: terminal`: the palette is drawn in cells, `❯` before the query, `▸` on drill-in rows; on one-dark the selected row is inverse video on the bar.
- On KDE/Breeze: the native rows' selected row is the OS highlight pair (Breeze #3DAEE9 under white ≈ 2.5:1) — R-h's exemption cites macOS only.
- 2026-10-26 23:52:44 Finish Steps 1–5 committed: 020dee833 (re-review minors a–e), c05d77a10 (/simplify, opus fan-out), 19cd049ec (display-pass fixes: PopupSurface built an idle shadow that greyed every dialog — now only with elevation, with a test; macOS query aligned with row titles; Terminal-row chip one cell from the prompt), 31dbd5236 (release notes). 448 PNGs; M8 not seen. Pictures sent to the owner.
- 2026-10-26 23:52:44 Final whole-step review dispatched (code-reviewer, opus) over d8bdfa7df..31dbd5236, incl. the untraced "DelegateModel::cancel: index out range 9 1" warning.
- 2026-10-27 00:04:12 Final review: Ready after fixes — I1 (a resting pointer takes the selection after every model reset), M1 (tabs sort 1, 10, 2), M2 (PaletteColorRoleEnd before 3b's roles land), M3 (refresh while open desyncs field and list), M4 (warning explained). Review: final-review.md.
- 2026-10-27 00:04:12 Ruling: one fix wave takes I1, M1 (keep the tab source's position order for Switch To Tab; numeric-aware compare elsewhere), M2, M3 (skip refresh while the palette is open, and sync the field on filterChanged), M4 (one frame before filtering in the scene). The adjacent clang-tidy finding goes to Follow-ups.
- 2026-10-27 00:34:42 Final fix wave committed: 872a51d58 (I1), f4b922bf6 (M1), ab00b72c0 (M2), 96a0c3894 (M3), c5de359d9 (M4); filters pass offscreen. 274 lines, so a scoped re-review agent (sonnet).
- 2026-10-27 00:35:35 Re-review of the fix wave: all addressed. Residuals adjudicated: (1) the first open with a resting pointer still lets one synthetic hover through — fixed now (seed lastPosition on aboutToShow, with a test); (2) shown cleared on destruction — fixed now; (3) naturallyBefore's index loops — accepted (not C-style for loops; a tokenizer would not read better).
- 2026-10-27 00:39:31 Residual fix f9afb0c3c controller-checked (hover judged in scene coordinates, first event after open/enter only records; onDestruction clears shown): OK. Task 8: complete. Step 3d: done.
