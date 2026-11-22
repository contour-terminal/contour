# Orchestrator attention: implementation plan (Fleet + Focus, phase 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** The dashboard says where to look without being asked: a run that starts to need the user is
named on the action bar and raises a desktop notification while the dashboard is not focused, the
dashboard's tab title counts the runs that need the user, a run (and a Focus tab) with news since it was
last looked at carries a `•`, and working rows turn a spinner.

**Architecture:**
- **No host change.** Everything goes out in the frame the dashboard already writes through `tab-write`:
  - the notification is an OSC 99 desktop notification with `o=unfocused`, which Contour raises only while
    the dashboard's pane lacks focus (`isNoticeWanted`, the existing path every application notification
    takes);
  - the tab title is OSC 2, `Agents · 2!`, written whenever the count changes (and in the first frame).
- **Needs-you entries:** the dashboard keeps the runs that were in Needs you when it last built its rows.
  The first build only primes the set: a run already waiting when the dashboard opens raises nothing. A run
  that enters it afterwards (from any group; moving between Asking and Waiting is not an entry) raises one
  notification and becomes the action bar's right-end notice, `pr-2375 needs you · home`, until it is
  selected or leaves Needs you.
- **Unseen markers (`view/Attention.{hpp,cpp}`):** a `SeenLedger` per dashboard, in memory.
  - A run's Fleet news compares its `RunMarks` (row state, the time of its last state change, its
    transcript's last timestamp from `agent-stats`) with the marks as last seen. A run first met is seen as
    it is; the selected run is always seen.
  - The selected run's tabs: the Board tab has news when its board's modification time moved since the
    Board tab was last shown; the Transcript tab when its last page grew since the Transcript tab was last
    shown. A mark that is not known yet (no board read, an earlier page shown) is never news and never
    overwrites what was seen.
  - The rows carry `News`, set by the dashboard before each draw; the Focus frame carries the tabs' news.
- **Spinner:** the working look's mark is the first Dots frame; the dashboard owns a core-cpp `Spinner` on
  its clock and ticks it once per drawn frame, so it turns on the redraws that happen anyway (an age moving
  on, a followed transcript, a key) and asks for no wake of its own.

Spec: `docs/superpowers/specs/2026-11-22-orchestrator-fleet-focus-design.md`, "Attention" and "Visual design".

## Global constraints

- No extra timer: `nextRepaintDue()` is unchanged by the spinner and the markers.
- The notification and the title carry only sanitised text (`cut()`), never a control character.
- `3`'s unpersisted hold keeps a run in Needs you (Asking → Waiting is no entry, so no second notice).
- Zero warnings, clang-tidy clean, `-j4`. Goldens rewritten only deliberately.

## File map

| File | Change |
| --- | --- |
| `view/Attention.{hpp,cpp}`, `view/Attention_test.cpp` | `RunMarks`, `News`, `SeenLedger`, `needsYouEntries()`, the OSC 99 / OSC 2 writers |
| `view/Fleet.cpp` | the working mark is `⠋` |
| `view/Sections.hpp` | `Row::news` |
| `view/Focus.{hpp,cpp}` | `FocusFrame::tabNews`; `•` after a tab's label |
| `view/Dashboard.cpp` | the spinner, the ledger, the needs-you set, the right-end notice, the frame's OSC 99 and OSC 2 |
| `view/Dashboard_test.cpp`, goldens | |
| `src/vthost/AgentRun_test.cpp` | e2e: the dashboard's terminal is titled `Agents · 1!` once an agent asks |
| `README.md`, `docs/extensions.md` | |

## Task 1: the spinner

- [ ] `⠋` for the working look; the dashboard's `Spinner` ticks per drawn frame; working rows draw its frame.
- [ ] Test: frames advance only on redraws; `nextRepaintDue()` unchanged.

## Task 2: needs-you notices

- [ ] `needsYouEntries(before, now)`; the dashboard primes, then notices entries.
- [ ] The right-end notice, cleared on selection or exit; OSC 99 per entry; OSC 2 on a changed count.
- [ ] Tests: no notice on open; one per entry, none for Asking → Waiting; the title's count; the bar text.

## Task 3: unseen markers

- [ ] `SeenLedger`, `RunMarks`; `Row::news`, `FocusFrame::tabNews`; drawing the `•`.
- [ ] Tests: a state change on another run marks it; selecting clears it; a board read while on Transcript
      marks the Board tab; first sight marks nothing.

## Task 4: e2e, docs and simplify

- [ ] vthost: the dashboard's window title after an agent asks.
- [ ] README (Attention), docs/extensions.md (no new host call; OSC 99 and OSC 2 in the frame).
