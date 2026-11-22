# Orchestrator actions and palette: implementation plan (Fleet + Focus, phase 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** What the dashboard offers adapts to the selected run. One table says which actions apply to each run
state and in what order; the action bar, `⏎`, the boxes at the bottom of the Focus and the command palette all
read it. The palette (`^K`, `?`) lists every action, replaces the help, and holds the commands that lost their
keys: refresh, install the missing plugins, and the 24-hour timeline.

**Why now:** phase 2 left the bar listing the key table's hints filtered by the row's offers, `⏎` as a switch
over row kinds, and `R`, `I` and `?` as stand-in keys until the palette existed.

**Architecture:**
- `view/Fleet.{hpp,cpp}` gains `RowState` (asking, waiting, errored, starting, working, done, ending, finished,
  kept, new agent, group heading, leftover worktree, none) and `rowStateOf(Run)`. `buildRows()` stores each
  row's state in `Row::rowState`, so nothing recomputes it per frame.
- `view/Actions.{hpp,cpp}` holds the two tables:
  - `CommandRows`: per command, its label and whether it acts on the selected row or everywhere; the palette's
    order;
  - `ActionRows`: one row per (state, command), in rank order, with the label `⏎` uses where it differs
    (`reply`, `message`, `follow up`, `run again`, …).

  From them: `actionsFor(state)`, `primaryAction(row)`, `applies(row, command)` (the table and the row's offers),
  `whyNot(row, command)` (the reason the palette and the bar show), `keyNameOf(command)` from the `KeyRow`
  table, and `barHints()` for the bar and the boxes.
- `view/Palette.{hpp,cpp}`: `paletteEntries()` (the row's actions, then the global commands, then the row
  commands that do not apply, dimmed with the reason), fuzzy matching with core-cpp's
  `completer::FuzzyMatch`, the palette's keys, `PaletteView`, and `TimelineView`, the 24-hour timeline restored
  from `9bb8c6b0f` as a popup.
- `KeyRow` stays the only source of key → command; it loses its hints. `Interaction` gains
  `std::optional<PaletteState>` and the `Overlay::Timeline`, and loses `Overlay::Help` and the install
  confirmation.

**Deviations from the spec, decided here:**
- The palette is drawn by the orchestrator, like the launcher, not by `CommandPalettePopup`: that popup
  owns its state outside `Interaction`, runs callbacks, and has no dimmed entries with a reason. Its fuzzy
  matcher and its highlight drawing are reused.
- Choosing "install the missing plugins" in the palette installs them: the entry names the plugins, and
  reaching it takes `^K` and a choice, so it does not ask a second time as `I` did.
- Each state lists every action that applies after the spec's ranked ones (for example `r` on a working run),
  so a key keeps working where it did; the bar cuts to the width.
- The quick-reply chips (phase 4), "fold every group" and "show dismissed runs" are not part of this phase.

**Tech stack:** C++23, Catch2, core-cpp TUI (golden screens).

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Places and keys", "Actions: one
table, adapted to the row", "The command palette".

## Global constraints

- A letter never does two things: in the palette's query every printable key types.
- `⏎` acts, `esc` steps back and loses nothing: the palette's query survives `esc`.
- Every action says what happened: a key whose action does not apply says why on the bar.
- Zero warnings, clang-tidy clean, `enum class`. Goldens are rewritten only deliberately. Build with `-j4`.

## File map

| File | Change |
| --- | --- |
| `view/Fleet.{hpp,cpp}` | **add** `RowState`, `rowStateOf()` |
| `view/Actions.{hpp,cpp}` | **new:** the command and action tables and what derives from them |
| `view/Palette.{hpp,cpp}` | **new:** palette entries, matching, keys, `PaletteView`, `TimelineView` |
| `view/Keys.{hpp,cpp}` | `^K` `?` open the palette; `⏎` dispatches the primary action; `R` `I` go; gating by the table |
| `view/Sections.{hpp,cpp}` | `Row::rowState`; headings offer `Fold`; `countText()` public |
| `view/Focus.cpp` | the done and ended boxes list the table's actions; the primer names `^K` |
| `view/Dashboard.cpp` | the adaptive action bar; the palette and timeline overlays; the help goes |
| `core/Engine.cpp`, `view/DispatchForm.cpp` | the missing-plugin texts point at `^K` |
| goldens, `guest/Driver_test.cpp`, `src/vthost/AgentRun_test.cpp` | the new bar, the palette, refresh through it |
| `README.md` | the bar, the palette, the keys |

## Task 1: The tables

- [ ] `RowState`, `rowStateOf()`, `Row::rowState`.
- [ ] `Actions`: the tables, `actionsFor`, `primaryAction`, `applies`, `whyNot`, `keyNameOf`, `barHints`.
- [ ] Tests (`[actions]`): `rowStateOf` per run state; every state but none has a primary action; every action
  command but `⏎`-only ones has a `KeyRow`; `applies` follows the offers; `whyNot` names the reason.

## Task 2: Keys and the bar

- [ ] `⏎` runs the primary action; a key whose action does not apply says why.
- [ ] The action bar from `barHints()`, then `^k all actions`; the boxes from the same table.
- [ ] `R`, `I` and the help go; `?` and `^K` open the palette.
- [ ] Goldens rewritten and read; Keys and Dashboard tests.

## Task 3: The palette and the timeline

- [ ] `paletteEntries()`, matching, keys (type, `↑↓`, `⏎` runs, `esc` closes keeping the query).
- [ ] `PaletteView` and `TimelineView`; refresh, install plugins and the timeline as palette commands.
- [ ] Tests: ordering and dimmed reasons; running a command equals pressing its key; goldens at 120×40, 80×24
  and 50×20.
- [ ] The guest and vthost e2e refresh through the palette.

## Task 4: Docs

- [ ] README: the bar, the palette, the keys; the primer names `^K`.

## Verification

- [ ] `/simplify` over the phase.
- [ ] `orchestrator_test`, `vthost_test`, `vtextension_test`.
