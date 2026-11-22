# Orchestrator Fleet + Focus: implementation plan (phase 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** One screen to see, read and talk to agents. The Fleet (left) lists the runs by what they need from
you; the Focus (right) shows the selected run: its board, its transcript or its details, the latest of what it
said, and the box to write to it.

**Why now:** the launcher (phase 1) is the way to start an agent on an item, so the item lists, `d` and the task
letter keys can leave the dashboard.

**Architecture:**
- `view/Fleet.{hpp,cpp}` groups the runs by attention: a data table of the four groups (Needs you, Working,
  To review, Finished), the group of a run, the order inside a group, and what a board says of its progress.
  `buildRows()` in `view/Sections.cpp` builds the Fleet's rows from it; the item, timeline and orphan sections
  go (orphans stay as an exceptional `LEFTOVER WORKTREES` group, see the deviations below).
- `view/Focus.{hpp,cpp}` draws the Focus as one component: the header, the tab strip, the tab (reusing
  `StatusBoardPanel`, `TranscriptPanel` and the detail lines), `latest`, and the bottom box; the empty-Fleet
  primer; and the furniture plan that decides what gives way in a small tab.
- `Interaction` loses `screen`, `subject`, `back`, `pane`, `overlay`-for-details and `message`; it gains
  `Place {Fleet, Focus}`, the chosen `FocusTab` that sticks per run until its state changes, the Focus scroll
  and follow, the transcript page and the per-run drafts.
- The transcript of the Focus run is read by the dashboard through the existing `ReadTranscriptAction`
  (`agent-transcript`, one call per run in flight in the engine): on selection, again every five seconds while
  the run is live, and at once when the read says the log has grown a page. `latest` and the Transcript tab
  draw the same page.
- `o` copies the run's link with OSC 52 in the next frame; every URL drawn is an OSC 8 hyperlink through
  `Canvas::addHyperlink()`, which core-cpp's `HyperlinkEmitter` frames on flush.

**Tech stack:** C++23, Catch2, core-cpp TUI (golden screens).

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Layout", "Left pane", "Right
pane", "Bands" and "Places and keys".

## Global constraints

- A letter never does two things: in the Fleet (and a Focus without a message box) letters are commands; in
  the message box, the filter and the launcher every printable key types.
- `⏎` acts, `esc` steps back and loses nothing: a draft survives `esc`.
- The action table, the adaptive action bar and the palette are phase 3: the bar lists the key table's hints
  that apply to the selected row. `R`, `I` and `?` stay as keys until the palette takes them.
- Zero warnings, clang-tidy clean, `enum class` and `std::expected`. Goldens are rewritten only deliberately.
  Build with `-j4`.

## File map

| File | Change |
| --- | --- |
| `view/Fleet.{hpp,cpp}` | **new:** `FleetGroup`, the group table, `fleetGroupOf()`, `waitingSinceOf()`, `boardProgressOf()` |
| `view/Focus.{hpp,cpp}` | **new:** `FocusTab`, `openingTabOf()`, `FurniturePlan`, the `Focus` component |
| `view/Sections.{hpp,cpp}` | `buildRows()` builds the Fleet; `Row` gains the second line and the meter |
| `view/Keys.{hpp,cpp}` | the place model; `b` `t` `d` tabs, `o`, `r`, `tab`; the message box; `Screen` goes |
| `view/Dashboard.{hpp,cpp}` | the layout per band, the top bar, the action bar, follow, drafts, OSC 52 |
| `view/StatusBoard.*`, `view/Transcript.*` | draw inside the Focus, from a `FocusSubject` rather than the interaction |
| `view/Theme.{hpp,cpp}` | the spec's palette, 256-colour by default, truecolor on request; roles `Working`, `Focus` |
| `src/vthost/AgentRun_test.cpp`, `src/vthost/testing/OrchestratorDashboard.hpp` | the e2e keys and texts |
| goldens | Fleet + Focus at 120×40, 80×24 and 50×20; the empty Fleet; each tab; the message box |
| `README.md` | the dashboard and its keys |

## Task 1: Theme

- [ ] The palette table: truecolor and 256-colour per role; `dashboardTheme(ColorDepth)`.
- [ ] Roles `Working` and `Focus`.

## Task 2: Fleet

- [ ] `FleetGroup` table and `fleetGroupOf()`: `idle`, `blocked` and `error` in Needs you; no report yet in
  Working as `starting`; done in To review; finished and kept in Finished, folded to the newest five.
- [ ] Order inside a group: longest waiting first; Finished newest first.
- [ ] Rows: glyph, run, task, state and age; the second line (the reported message, the board's first open
  `☐`, the failure words); a `▰▱` meter where a board was read; `+ new agent` last.
- [ ] The selection follows its run across a rebuild.
- [ ] Tests: grouping and ordering, the selection kept across a regroup.

## Task 3: Focus and places

- [ ] Header (two lines), tab strip, tab body, `latest`, bottom box; the primer.
- [ ] Opening tab per state; a chosen tab sticks until the state changes.
- [ ] Keys: `⏎`, `tab`, `esc`, `b` `t` `d`, `pgup` `pgdn` `end`, `n`, `/`, `e` `x` `del` `z` `r` `o`.
- [ ] The message box: type, `⏎` sends, `alt+⏎` adds a line, `esc` keeps the draft; after a send the keys
  return to the Fleet; a refusal puts the draft back.
- [ ] Bands and the furniture order.
- [ ] Goldens and Keys tests.

## Task 4: Transcript follow

- [ ] The Focus run's last page is read on selection, every five seconds while it is live, and at once when the
  log has grown a page; pinned to the end until the user scrolls up, `↓ live` while away, `end` returns.
- [ ] Test: pinning and unpinning on scroll; the re-read schedule.

## Task 5: Top bar, links

- [ ] Top bar: brand, `⬢ n/max ▰▱`, `● N need you`, the setup warning.
- [ ] OSC 8 on every URL drawn; `o` copies with OSC 52.

## Task 6: e2e and README

- [ ] vthost `[agents]`: open the launcher with `n` and a query; the runs' rows; the new texts.
- [ ] README: the dashboard sections and the key table.
