# UI/UX rules

Apply these to every change that adds or alters something a user sees or touches: chrome, popups, pickers,
dialogs, the sidebar, menus, tooltips, the Settings window. They sit on top of
[docs/internals/native-chrome.md](../../docs/internals/native-chrome.md) — the token model, the four chrome
rows, the contrast numbers, "nothing over non-blank cells". Follow that; do not restate it.

The test behind every rule: **would a user of this platform, who never read our docs, do the right thing on
the first try — and undo it the same way?** Each rule names how a reviewer checks it.

## 1. Native look comes from tokens, per row

1. Take every extent, radius, colour, type size, duration and glyph from `chromeStyle`. No literals, glyphs
   included (`"▾"`, `"▸"`, `"☰"`). *Check:* `ctest -L lint` passes; `grep -nE '"[▸▾☰›✕•]"' src/contour/qml/*.qml`
   finds nothing new.
2. Draw a control the platform has the way the platform does: themed symbolic icons in the ContourLinux
   header bar (an `iconName` plus a bundled fallback), SF-Symbol-shaped icons on macOS, Fluent's shapes on
   Windows, cells on the Terminal row. Never put a text glyph beside an icon in the same bar.
   *Check:* the control's face comes from a table (`HeaderBarItems`, `SidebarControlRows`).
3. One glyph means one thing per row: not the main menu's hamburger for "toggle sidebar", not the new tab's
   `+` for "new workspace" in the same place. *Check:* a Catch2 case compares the faces.
4. Selection, hover and pressed follow the row's own convention, through one component and the row's tokens
   — no per-platform QML forks. A list's selection matches the command palette's on the same row.

## 2. Whatever hides something brings it back, from the same place

1. A control that hides, collapses or dismisses a surface stays on screen at the same position, and its
   second press restores the surface. Its name and tooltip flip ("Hide sidebar" / "Show sidebar"), and it
   exposes `Accessible.checkable` / `Accessible.checked`.
2. If it cannot stay (it lived inside what it hid), an identical control appears at the same spot in what is
   now there. *Check:* a test clicks it twice through `findChild` and asserts the surface is back.
3. Every hide also has a keyboard path (an action with a default chord) and a menu or palette path.
4. Check every layout's "off" state (no band, full screen, tabs on top, a defaults edition) for a pointer way
   back. Where no toggle can be on screen, a menu that always can (the pane's context menu) offers it.

## 3. Title regions move the window

1. Every empty area of the top band — a sidebar's title row, the header bar's part above it, any segment
   beside the tabs — drags the window, maximises on double-click and opens the window menu on right-click.
2. Use `TitleBarGestures.qml`, never a hand-rolled `DragHandler`: its grab permissions are load-bearing
   (issue #1997). Controls on the band keep their own press (`ToolbarButton` forbids take-over).
3. *Check:* a test finds the gesture handler under every band segment the layout can produce.

## 4. Popups and pickers are one family

1. A searchable list is the palette: an in-scene popup on `PopupSurface`, `radii.popover`, `elevation.e3`,
   the palette's query row, drawn hint, rows, selection, empty state and footer. Factor shared parts into
   components; never copy a row between files.
2. A popup opened from a control (a chip, a `▾` button) hangs from that control (`window::popupBelow`); one
   opened from a chord or the palette is centred like the palette.
3. No new top-level OS window for a transient choice — only for something kept open beside the terminal
   (Settings) or the pre-window Connect picker.
4. Footer key hints come from the model, rendered per platform (`command/Shortcut.hpp`), never a hard-coded
   "Return = …" string.
5. Draw a field's hint yourself (as `CommandPalette.qml` does), not with `placeholderText`, which puts the caret
   on the hint's first letter.

## 5. Keyboard parity

1. Every pointer action has a keyboard path; every keyboard-only action (Shift+Return = "open in new window")
   has a pointer path (a row menu, a button).
2. Lists and trees: ↑/↓ move, Home/End jump, Return activates, Escape backs out one level then closes, ←/→
   collapse/expand, the Menu key and Shift+F10 open the row's menu, F2 renames where double-click does.
   *Check:* the area's rows in `window::ChromeKeyRows` and a `[chromekeys]` case.
3. Focus returns to where it was when a popup, editor or picker closes.
4. A new default chord is checked against the shell (`Ctrl+\`, `Ctrl+J`, `Ctrl+D`, …) and the
   chord-uniqueness test; name any clash in the change description.
5. A control's tooltip shows the shortcut its action is bound to *now* — the configured or default chord, read
   from the binding at runtime (`toolTipCommand` → `KitToolTipHost`), never written into the text — and follows
   a rebinding. A control without an action is a finding: give it one, so it can be bound, sits in the command
   palette and shows its chord. Unbound: the plain label, no stray separator; never invent a default chord to
   fill the tooltip. *Check:* a QML case per control that the tooltip shows the bound chord and drops it when
   unbound.

## 6. Every state is designed

1. Rest, hover, pressed, focus, selected, disabled, empty, loading, error — each is drawn on purpose. Hover
   is `tabHoverFill`, pressed `controlPressedFill`, on the rows whose platform shows them; focus is
   `FocusRing`, distinct from selection.
2. On macOS a list's selection is emphasized only while the list has the keyboard; otherwise it is the
   unemphasized grey (`*SelectionFillInactive`).
3. Disabled looks disabled and is announced so.
4. Hide counts that are zero; never show "0 need you". An empty list says what to do next.
5. Async work shows progress inside the surface and never blocks typing.
6. Plurals: one source string per form chosen by count (`n == 1 ? tr("%n pane needs you") :
   tr("%n panes need you")`), so English reads right without a loaded translation. Never glue a number to a
   fixed noun.

## 7. Motion respects Reduce Motion

1. Every `Transition`, `Behavior` and animation reads its duration from `chromeStyle.motion`, so Reduce Motion
   collapses it; spinners become static glyphs.
2. Nothing animates layout on every model update (a 1 Hz tick must not re-run an `add` transition).

## 8. Accessibility: roles, names, states

1. Set `Accessible.role`, `.name` and, where relevant, `.description`, `.checkable`/`.checked`, `.selected`
   and `.focusable`. A tree is `Accessible.Tree` with `TreeItem`s, not a list.
2. The name is what the item shows; kind and status go in the description; selection and checked are
   states. Never bake state words into the name. A state Qt Quick's `Accessible` cannot carry (Qt 6.11 has
   no expanded state) goes in the description, translated.
3. Every word a screen reader hears is translatable (`tr()`, `qsTr()`), built in C++ where the decision is.
4. Decorative glyphs set `Accessible.ignored: true`. *Check:* `test/Accessibility_qml_test.cpp` lists the item.

## 9. QML decides nothing

1. Every rule — where a control stands, what a row says, which state wins — is a pure, Qt-free C++ function
   or table with a Catch2 test. QML lays out and binds.
2. A `bool` in a decision's API is an `enum class` (AGENT.md); QML-facing `Q_PROPERTY` bools are the
   documented carve-out.

## 10. Snapshots on all four rows, and a look at them

1. Add a script to `src/contour/test/ChromeSnapshotPlan.hpp` for every new surface and each new state of it
   (shown/hidden, empty, filled, hover, focus, error, a popup open).
2. Render: `CONTOUR_SNAPSHOT_SCRIPT=<name> scripts/run-chrome-snapshots.sh <contour_gui_test> <dir>` — macOS,
   Fluent, ContourLinux and Terminal; light and dark; both editions.
3. Open the PNGs and look at them, beside the palette's and the tab strip's of the same row. Name the files
   you looked at in the change description. Snapshots are review artefacts, not acceptance.

## 11. The UX check before review

Answer each in the change description, one line each:

- **Undo:** what each new control hides or changes, and how the same control in the same place undoes it.
- **Drag:** which new parts of the top band exist, and that they move the window.
- **Family:** which component it reuses (palette, `PopupSurface`, `ToolbarButton`, `KitDialog`), or why none.
- **Keys:** the keyboard path for every pointer action, and the pointer path for every keyboard action.
- **States:** where hover, pressed, focus, disabled, empty and error are seen in the snapshots.
- **Motion:** what animates, and what it does under Reduce Motion.
- **AT:** role, name, description and states of every new interactive item.
- **Snapshots:** the PNG paths looked at, per row.
- **Platform:** one sentence per row on the convention it follows (macOS HIG, WinUI, libadwaita / Breeze),
  and any deliberate divergence.
