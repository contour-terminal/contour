# Native Redesign — Plans Index

> **For agentic workers:** Read this index first. Then pick the first step whose plan is written and
> whose dependencies have landed, and execute its plan with
> superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Deliver the native redesign in PR-sized steps, in a fixed order, each leaving the app
coherent.

**Spec:**
- `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` (the engineering
  design; the section column below cites it).
- `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (the approved visual design
  record).
- Canvas: <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>.

**Branch and PR:** `feature/native-redesign-phase0`, draft PR #2178.

## Status board and handoff (every step)

This run is tracked on a status board: <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>. The
owner asked for the link to live here so that any agent continuing the work can find the board and
keep it current. The page is kept with `/status-board` (the `contour-workflows` plugin) on the machine doing the work; its source is local to that working tree and never committed.
- **Keep it up to date** with `/status-board update`, which republishes to the link above
  (`/status-board` remembers the link per working tree). The board belongs to the owner's account, so a
  session signed in as the owner can read and publish it; the design canvas, the owner's copy of
  Yaraslau's, is in the same account.
- **Every agent that continues this work updates the board.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Resume here

Read these in order, then pick up the first open task:

1. This index: the step order, and the step table below with each plan's state.
2. The ledgers in `docs/superpowers/ledgers/`, one per plan. Each holds the plan's pre-flight
   scan, every ruling (binding on later work), each task's commits and review outcome, and the
   findings parked for a later step. They are copies of the subagent-driven-development ledgers,
   which live in the git-ignored `.superpowers/sdd/<plan>/progress.md`. Copy a ledger back there
   to resume a plan with superpowers:subagent-driven-development, and copy it out again when
   handing over.
3. The plan of the step in progress, and its task's brief.

Last update: 2026-10-25, Step 1b done (`c8fe3124`). Its ledger, with every ruling, the
controller-added Task 12a (⌘H, ⌥⌘H and ⌘M stay with the macOS menu; Minimize and Zoom; "Settings…"),
the final review and its fix wave, and the parked findings, is
`docs/superpowers/ledgers/2026-10-25-native-redesign-step-1b.md`. Next: write the Step 4a plan.
Carry into it from that ledger: About and Settings… open on a minimised window after ⌘M (Step 4a's
Settings window should raise or own its window), and the design canvas's macOS ⌘ chords (⌘N, ⌘T,
⌘W, ⌥⌘W, ⇧⌘W, ⌘D, …), which no edition binds yet (issue #2175).

## Rules for every step

- **Plans are written just in time.** When a step's dependencies land, write its plan with
  superpowers:writing-plans, from the cited design section and the code as it then stands.
  - Save it as `docs/superpowers/plans/YYYY-MM-DD-native-redesign-step-<id>.md`.
  - Add it to the table below, and update the board.
- **Every plan carries:**
  - this index's *Status board and handoff* section, copied into its header;
  - tasks in TDD order, each with its own test filter;
  - the Terminal (TUI) row's rendering for every visual task;
  - a Review Focus list.
- **Each task runs only its own tests** (owner ruling, 2026-10-24). The full suite runs in CI on
  Linux, macOS and Windows.
  - On a machine without `cargo`, configure with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`.
- **Commits** are one per task, and end with the attribution line the session's instructions give.
- **AGENT.md applies throughout**: `enum class` over `bool`, `std::expected`, dependency
  injection, configuration at construction, data-driven tables, zero warnings, and `/simplify`
  before a step is called done.

## Steps

| Step | Delivers | Depends on | Design § | Plan |
|---|---|---|---|---|
| 0 | Foundations: ChromeStyle rows, shortcut glyphs, `theme: terminal`, contour-dark/light/classic, no single-pane focus frame, QML colour lint, bundled JetBrains Mono, first-launch defaults behind `DefaultsEdition` | — | 3.0, 2.7 | `2026-10-23-native-redesign-phase0.md` (done) |
| 1e | Chrome snapshot driver (test-only, `[display]`) | 0 | 2.5 | `2026-10-24-native-redesign-step-1e.md` (done) |
| 1a | Kit primitives (Toast, Banner, KitDialog), token groups, size-literal lint, accessibility preferences and platform appearance (null + macOS), Reduce Motion for terminal durations, the new-defaults banner, `hostingNoticeBar` → Banner | 1e | 3.1, 2.1, 2.3, 2.4 | `2026-10-25-native-redesign-step-1a.md` (done) |
| 1b | Menus as data, `NativeMenuPresenter` (macOS), `MenuBarTable` + `AppMenuBar` | 1a | 3.2 | `2026-10-25-native-redesign-step-1b.md` (done) |
| 4a | Settings window shell; pseudo-tab and `settingsActive` removed; the `settings` row of the snapshot table (`SnapshotScripts`) changes in the same commit | 1b | 3.3 | `2026-10-25-native-redesign-step-4a.md` (done) |
| 1c | Title band, tab well and pills, badge slot, HostChip, drift chip, tab colour, no-title-bar and full screen | 4a | 3.4 | `2026-10-25-native-redesign-step-1c.md` (written) |
| 1d | macOS title-band material and safe area (spike first) | 1c | 3.5 | `2026-10-25-native-redesign-step-1c.md` (written) |
| 2 | Pane cards, headers, gap and hit area, densities, pane states | 1d | 3.6 | `2026-10-25-native-redesign-step-1d.md` (written) |
| 3a | Status line placeholders, flags, role colours, collapse, final template | 2 | 3.7 | `2026-10-25-native-redesign-step-2.md` (written) |
| 3b | Find bar: regex, whole word, chunked search, placement | 2 | 3.8 | `2026-10-25-native-redesign-step-2.md` (written) |
| 3c | NoticeModel, permission Dialog, guarded-role enums, consent, Save Layout | 2 | 3.9 | `2026-10-25-native-redesign-step-2.md` (written) |
| 3d | Command palette grouping and drill-in | 2 | 3.10 | `2026-10-25-native-redesign-step-2.md` (written) |
| 3e | Size indicator, overlay scrollbar, link pill (+ tooltip default), hint labels | 2 | 3.11 | `2026-10-25-native-redesign-step-2.md` (written) |
| 3f | Command blocks: rail, chip, block menu, fold chip, sticky header as chrome, ticks | 1b, 2, 3e | 3.12 | `2026-10-25-native-redesign-step-3f.md` (written) |
| 4b | Settings sections, controls, one save model, `option_as_alt` per side, live preview | 3a–3f | 3.13 | to write |
| 5a | Windows 11: Mica, Snap Layouts, Fluent row, Windows backends, menu decision | 4b | 3.14 | to write |
| 5b | Linux: ContourLinux style, header bar, portal backends | 4b | 3.15 | to write |
| 6.1–6.10 | Moments: one PR each | 5a, 5b | 3.16 | to write |

Steps 3a to 3e may run in any order, or in parallel worktrees, each with its own board.
