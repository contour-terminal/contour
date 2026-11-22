# Orchestrator attention: implementation plan (Fleet + Focus, phase 5)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for
> tracking.

**Goal:** The dashboard says where to look without being asked: a run that starts to need the user is
named on the action bar, the status-line summary counts the runs that need the user, a run (and a Focus tab)
with news since it was last looked at carries a `•`, and working rows turn a spinner.

**Architecture:**
- **No host change.** The spec asks for a tab title written as OSC 2 and a notification through the existing
  path "if the host offers one". It does not: a served tab executes only the sequences that draw
  (`vtbackend/vt/ServedTabSequences.hpp`), so a title, a notification (OSC 99) and BEL written through
  `tab-write` are dropped by design, and a served tab's preset name would win over a title anyway. The count
  reaches another tab through the existing `summary-set` instead: its `●N blocked` term becomes `●N need
  you`, counted by one `needsUser()` predicate the Fleet's grouping shares.
- **Needs-you entries:** the dashboard keeps the runs that were in Needs you when it last built its rows.
  The first build only primes the set: a run already waiting when the dashboard opens is not named. A run
  that enters it afterwards (from any group; moving between Asking and Waiting is not an entry) becomes the
  action bar's right-end notice, `pr-2375 needs you · home`, until it is selected or leaves Needs you.
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
- `3`'s unpersisted hold keeps a run in Needs you (Asking → Waiting is no entry, so no second notice).
- Zero warnings, clang-tidy clean, `-j4`. Goldens rewritten only deliberately.

## File map

| File | Change |
| --- | --- |
| `view/Attention.{hpp,cpp}`, `view/Attention_test.cpp` | `RunMarks`, `SeenLedger`, `needingRuns()`, `needsYouEntries()` |
| `core/Finished.{hpp,cpp}` | `needsUser()`; the summary's `●N need you` |
| `view/Fleet.cpp` | the working mark is `⠋` |
| `view/Sections.hpp` | `Row::news` |
| `view/Focus.{hpp,cpp}` | `FocusFrame::tabNews`; `•` after a tab's label |
| `view/Dashboard.cpp` | the spinner, the ledger, the needs-you set, the right-end notice |
| `view/Dashboard_test.cpp`, goldens | |
| `src/vthost/AgentRun_test.cpp` | e2e: an agent that asks while another row is selected is named |
| `README.md`, `docs/extensions.md` | |

## Task 1: the spinner

- [ ] `⠋` for the working look; the dashboard's `Spinner` ticks per drawn frame; working rows draw its frame.
- [ ] Test: frames advance only on redraws; `nextRepaintDue()` unchanged.

## Task 2: needs-you notices

- [ ] `needsYouEntries(before, now)`; the dashboard primes, then notices entries.
- [ ] The right-end notice, cleared on selection or exit; the summary's `●N need you`.
- [ ] Tests: no notice on open; one per entry, none for Asking → Waiting; the summary's count; the bar text.

## Task 3: unseen markers

- [ ] `SeenLedger`, `RunMarks`; `Row::news`, `FocusFrame::tabNews`; drawing the `•`.
- [ ] Tests: a state change on another run marks it; selecting clears it; a board read while on Transcript
      marks the Board tab; first sight marks nothing.

## Task 4: e2e, docs and simplify

- [ ] vthost: the notice through the real guest.
- [ ] README (Attention).
