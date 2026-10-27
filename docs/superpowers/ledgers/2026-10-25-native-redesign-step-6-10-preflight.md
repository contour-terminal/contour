# Pre-flight scan: Step 6.10 (IME: the candidate window at the cursor cell, an inline preedit with a 1-pt underline)

Plan: `docs/superpowers/plans/2026-10-25-native-redesign-step-6-10.md` (written at `661b6029`).
Spec: §3.16 row 6.10 (binding), with §1, §2.6, §2.7. Rules: `AGENT.md`. Ledger:
`.superpowers/sdd/2026-10-25-native-redesign-step-6-10/progress.md` (D1).

Scanned against `work/native-redesign-step-2` at `324c198c` and the main line `0595bfda`. 3e and 3f (which touch
`EditionDefaults` and `GridMetrics.hpp`) are read from their plans and rulings. Nothing was built or run.

**Counts:** 4 tasks; 2 blocking findings, 12 non-blocking; 0 spec asks without a task; 2 owner questions (beyond the
plan's two).

---

## 1. Tasks that share a file or an interface

### 1a. Inside 6.10

| Pair | Shared file / interface | Status |
|---|---|---|
| 2 → 3 | `vtbackend::CellFlag::InputMethodPreedit` (bit 21) → the decorator table | Consistent; the decorator count (**F2**) |
| 1 ↔ 2 | none: Task 1 is `contour/display/`, Task 2 is `vtbackend` + config | Independent |
| 2 → 4, 3 → 4 | the filters in Finish | **F10** |

### 1b. Across steps and moments

| Pair | Shared file / interface | Status |
|---|---|---|
| 3e, 6.6 ↔ 6.10 T2 | `EditionDefaults` (`Config.hpp:1036`) and `EditionDefaultsTable` (designated rows); `Config_test.cpp` `ExpectedEditionDefaults` (`:4005`, `:4031`, `:4064`) | Each adds a column; the test table too (**F13**) |
| 3f ↔ 6.10 T3 | `GridMetrics.hpp` (3f adds `GutterRailMetrics rail` at `:39-60`), its formatter, `Renderer.{hpp,cpp}` | Sequential, additive (**F11**) |
| 4b ↔ 6.10 T3 | `vtrasterizer::DecoratorNames` feeds the configuration tokens and 4b's option list for link decorators | **F6** |
| 4b ↔ 6.10 T2 | Settings page rows for profile keys (`input_method_editor_style` would want a row) | Not planned in either; non-blocking |
| vthost ↔ 6.10 T2 | `config::sessionSettings` (`Config.cpp:965`) is shared by GUI and daemon-hosted sessions | **F4** |
| 6.6 ↔ 6.10 T2 | `Config.hpp`, `Config.cpp`, `ConfigDocumentation.hpp` | Disjoint members |

---

## 2. One row per task

| Task | References | Verdict |
|---|---|---|
| 1 Anchor | `ImeQueryRect.hpp` (`imeCursorAddressable` `:29-33`, `imeCursorRectangle` `:49-59`) ✓; `TerminalDisplay.cpp` `geometryChange` `:477-488` ✓, `inputMethodEvent` `:1447-1461` ✓, `inputMethodQuery` `:1464-1533` (the `ImCursorRectangle` case `:1489-1505`) ✓, `notifyCellGeometryChanged` `:1604-1617` ✓, `reportCursorMoved` precedent `:1733-1741`; `inputMethodQuery` is `const` (`TerminalDisplay.hpp:167`); `ImeQueryRect_test.cpp` `[contour][ime]` ✓; `Terminal.IME.CursorVisibleDuringComposition` and "Terminal.IME queries answered under the state lock …" `[terminal][ime]` (`Terminal_input_test.cpp:53`, `:425`) ✓; `scrollToBottomOnInput` (`Terminal.cpp:1148-1160`) ✓ | **F1**, **F7** |
| 2 Inline preedit | `CellFlags.hpp` (last bit `WrapPadding, 20`; 21 is free) ✓; `Settings` struct `Settings.hpp:114` ✓; `renderUtf8Text` `RenderBufferBuilder.cpp:935-990` (only the IME calls it) ✓; `tryRenderInputMethodEditor` `:992-1022` ✓; `TerminalProfile::inputMethodEditor` `Config.hpp:1228` ✓ (a profile key, as the style should be); profile loader `Config.cpp:1772`; `ConfigDocumentation.hpp` `InputMethodEditorSupportConfig` `:840` | **F3**, **F4**, **F13**, **F14** |
| 3 1-pt line | `Decorator.hpp` enum `:18-41`, `DecoratorNames` `:49-59`, `numeric_limits<Decorator>` `:94-103`; `DecorationRenderer.cpp` table `:37-46`, `dottedPhaseIndex` `:52-57`, `DirectMappedDecorationCount`, `createTileData` `:163`; `GridMetrics.hpp` `underline` `:45-49`, formatter `:121`; `Renderer.cpp` `loadGridMetricsFromFont` `:43-59`, published copy `:651`; `DecorationRenderer.underlineFamilyStaysBelowTheBaseline` (`DecorationRenderer_test.cpp:257`) ✓ | **F2**, **F6**, **F8**, **F9** |
| 4 Finish | `[display]` twice, unfiltered | **F10** |

---

## 3. Findings

### Blocking

**F1 — The display cases cannot run offscreen (Task 1).**
> "new `src/contour/display/TerminalDisplayIme_test.cpp` (… the new file drives a `TestApp` session's display
> offscreen)" … "a `QInputMethod` spy (the item's `updateInputMethod` reaches `QGuiApplication::inputMethod()->update`)
> counts one update after a position-only `setX`"

*Why.* `inputMethodQuery` returns the base class's answer while `_renderer` is null (`TerminalDisplay.cpp:1466-1469`),
and the renderer is created on the first scene-graph sync (`createRenderer()`, `:822-828`), which the offscreen QPA
never runs. That is why the existing IME display cases are `[display][ime]` with `REQUIRE_DISPLAY_OR_SKIP`
(`DisplayRendering_test.cpp:2470-2488`). `QQuickItem::updateInputMethod` is a no-op without active focus and emits
nothing a spy can count. The owner rule is offscreen per task and one targeted display pass in Finish.
*Resolution:* extract the decision. `imeAnchorCell` (planned) plus a pure
`[[nodiscard]] QRectF imeAnswer(std::optional<QRectF> computed, QRectF lastGood)` and a `ImeRectangleTriggers` table
(which geometry and preedit events re-announce), all in `ImeQueryRect.hpp`, tested in `ImeQueryRect_test.cpp`. Keep one
`[display][ime]` case (end-of-line and resize answers) for the Finish pass.

**F2 — The new decorator collides with the dotted underline's phase tiles (Task 3).**
> "`src/vtrasterizer/Decorator.hpp:18-41` — `PreeditUnderline`, appended."

*Why.* `std::numeric_limits<Decorator>::max()` is hard-coded to `Decorator::Encircle` (`Decorator.hpp:98`), and
`dottedPhaseIndex(p)` puts phase `p ≥ 1` at `count() + p - 1` (`DecorationRenderer.cpp:52-57`). Appended without
moving `max()`, `PreeditUnderline`'s own slot (9) is phase 1's slot, so one tile overwrites the other in the direct
mapping, and `DirectMappedDecorationCount` is one short. *Resolution:* set `max()` to `PreeditUnderline` in the same
commit, and add a test that every decorator's index and every dotted phase index are distinct and below
`DirectMappedDecorationCount`.

### Non-blocking

**F3 — Wrong loader line (Task 2).** "`Config.cpp` (load beside `input_method_editor`, `:2125`; write back)". `:2126`
loads the **colour scheme's** `input_method_editor` colours (beside `search_highlight`, `hint_label`); the profile key
is loaded at `:1772` (`loadFromEntry(child, "input_method_editor", …)`). Load and write `input_method_editor_style`
there.

**F4 — The copy site is `config::sessionSettings`, and a reload needs a row (Task 2).** "`TerminalSession.cpp` (copy
into the terminal's settings where `inputMethodEditor` support is applied)". `inputMethodEditor` is applied in
`TerminalDisplay.cpp:302` (an item flag), not in the terminal's settings. A terminal is born from
`config::sessionSettings` (`Config.cpp:965`, shared with daemon-hosted sessions); a reload assigns only the members in
`ReloadedSettings` (`TerminalSession.cpp:6204-6227`). *Resolution:* set `preeditStyle` in `sessionSettings` and add
`&vtbackend::Settings::preeditStyle` to `ReloadedSettings`, with a reload case.

**F5 — Daemon-hosted panes.** The preedit is local (`Terminal::updateInputMethodPreeditString` on the client's mirror
terminal), so the style must reach the mirror's settings too. `sessionSettings` covers both; check the mirror builds
its settings from it.

**F6 — Keep the new decorator out of `DecoratorNames` (Task 3).** That table is the configuration vocabulary for
hyperlink decorations and 4b's option list. Leave `PreeditUnderline` out, add a test that `toDecorator("…")` cannot
produce it, and make the formatter print an internal name.

**F7 — `_lastImeRectangle` must be `mutable` (Task 1).** `inputMethodQuery` is `const` (`TerminalDisplay.hpp:167`).
Document it as GUI-thread state at the declaration, beside the off-thread early return (`:1478-1481`).

**F8 — The DPI is not where the underline is computed (Task 3).** `loadGridMetricsFromFont(FontKey, GridMetrics&,
Shaper&)` (`Renderer.cpp:43-59`) takes no DPI. Pass the font descriptions' DPI, recompute on the DPI/content-scale
path, and copy the new field where the published metrics copy `underline` (`:651`).

**F9 — Test tags.** `GridMetrics_test.cpp`'s cases are tagged `[vtrasterizer]`; the new ones may carry
`[gridmetrics]`, but say so, so the Finish filter matches only the new cases.

**F10 — Display runs exceed the owner rule (Task 4).** Step 3 runs the whole `[display]` suite twice. One targeted
pass: `[display][ime]` on both rows.

**F11 — `GridMetrics.hpp` is edited by 3f first.** 3f adds `rail` and a formatter line; 6.10 appends after it. The
designated `GridMetrics { … }` initialisers in tests stay valid because the new member has a default.

**F12 — Line references.** All of the plan's `TerminalDisplay.cpp` and `RenderBufferBuilder.cpp` ranges still hold
within two lines. `EditionDefaults` is `:1036`.

**F13 — The edition column's test table (Task 2).** `Config_test.cpp`'s `ExpectedEditionDefaults` (`:4005`) and both
expectations name every column; add `.preeditStyle`. 3e and 6.6 add columns to the same rows.

**F14 — Inline colours under reverse video (Task 2).** "the preedit's cells take the palette's default foreground
and background". `makeColorsForCell` applies DECSCNM, selection and the cursor. Under DECSCNM, raw defaults would
draw the preedit unreversed on a reversed screen (owner Q1).

---

## 4. Spec asks that no task delivers

None.

## 5. Owner questions (proposed defaults; the plan's two stand)

1. **The inline preedit under reverse video (F14).** *Default: it follows the screen (reversed under DECSCNM), so it
   reads as the text around it.*
2. **The new decorator's visibility (F6).** *Default: internal only; not a value of any configuration key.*
