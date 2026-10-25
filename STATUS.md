# Contour — status

The status board's source, committed with the branch so another agent can resume; rendered and published to the board linked from PR #2178. Times are local, from `date`, to the second.

**Last updated:** 2026-10-25 13:54:20

## Current plan: a native look for Contour on macOS, Windows and Linux

The redesign gives Contour native chrome on every desktop: a title band with tabs, pane cards, a real menu bar, a Settings window and a useful status line. It keeps one component tree and a first-class TUI style. The design and the engineering plan are in draft PR #2178. Work runs step by step on `feature/native-redesign-phase0`, one PR-sized step at a time. Phase 0, the foundations, is finished; Phase 1, the window chrome, has its snapshot driver and kit primitives, and menus as data (Step 1b) are under way.

**Decisions:**
- 2026-10-25 13:54:20: work resumed by a new agent from the branch alone. Task 11's three extra tests were redone and passed review (`cbcbb084`).
- 2026-10-25 13:54:20: on the owner's word, the branch was rebased onto master `974feaf1` and force-pushed. `84a597ff` (vthost `SOCK_CLOEXEC`) was dropped, and the vthost part of `fd34db81` with it: master's `14791d6e` already carries that fix, with an error check, so the PR no longer touches vthost. Every commit hash on this board is the rebased one.
- 2026-10-25 12:14:59: work paused on the owner's word.
- 2026-10-25 12:08:32: on the owner's word, this board's link is in PR #2178 and in every plan, and the run's ledgers are committed under `docs/superpowers/ledgers/` (`7403acc6`, brought up to date in `76ffa4aa`), so another agent can resume and keep the board current.
- 2026-10-25 03:15:52: Step 1a finished and pushed (`3a4e2402`).
- 2026-10-24 23:47:03: Step 1e finished and pushed (`78566783`).
- 2026-10-24 22:13:13: Phase 0 finished and pushed (`92b1c444`).
- 2026-10-24 14:35:23: TUI style on macOS keeps AppKit's real traffic lights; status line on by default; all eight proposed defaults approved (recorded in `6dd5cb0b`).
- 2026-10-24 13:04:06: PR #2176 merged; the redesign branch was rebased onto `4637e4a6`.
- 2026-10-24 14:35:23: each task runs only its own tests; the full suite runs in CI (first applied to `fade7fa7`).
- 2026-10-24 15:08:23: the implementation design passed three Fable review rounds and was committed (`41189e61`).
- 2026-10-24 15:09:15: draft PR #2178 opened.
- 2026-10-24 15:11:47: plans index and the reviewed Phase 0 plan committed (`a8b24aee`) and pushed.

| Phase | Steps done | State | Where it is |
|---|---|---|---|
| 0 · Foundations | 1/1 | done | 9 of 9 tasks, plus a whole-branch review; pushed to PR #2178 |
| 1 · Window: 1e snapshot, 1a kit, 1b menus, 1c title band, 1d material | 2/5 | running | 1e and 1a done; 1b at 12 of 13 tasks (11 done); 1c and 1d not yet planned |
| 4a · Settings window shell | 0/1 | queued | lands after 1b, before 1c |
| 2 · Panes, headers, densities | 0/1 | queued | waits for 1c and 1d |
| 3 · 3a status line, 3b find, 3c notices, 3d palette, 3e overlays, 3f blocks | 0/6 | queued | waits for Step 2 |
| 4b · Settings sections and controls | 0/1 | queued | waits for Phase 3 |
| 5 · 5a Windows 11, 5b Linux | 0/2 | queued | waits for 4b |
| 6 · Moments 6.1–6.10 | 0/10 | queued | waits for Phase 5 |

## Phase details

States: ✅ done · ▶ running · ○ queued.

### Phase 0 · Foundations — done, 9 of 9 tasks

| Task | State | Commits |
|---|---|---|
| 1 · Chrome style rows (MacOS, Fluent, ContourLinux, Terminal) | ✅ | `fade7fa7`, `23695706` |
| 2 · macOS shortcut glyphs ⌘⌥⇧⌃ | ✅ | `bb897254` |
| 3 · `theme: terminal`: chrome follows the terminal's light or dark | ✅ | `8bff48b0`, `0d3a954c` |
| 4 · contour-dark, contour-light and contour-classic schemes | ✅ | `e42883d5`, `8c840dbb`, `5ae2533a` |
| 5 · No focus frame around a single pane | ✅ | `5f0e565e`, `34b49630` |
| 6 · No colour literals in chrome QML; status colour tokens | ✅ | `24394e02`, `5f574cd7`, `d2524b2e` |
| 7 · Bundled JetBrains Mono | ✅ | `0fe55018`, `112338de` |
| 8 · First-launch defaults behind `defaults_edition` | ✅ | `030b4283`, `1d04a21a` |
| 9 · Simplify, release notes, whole-branch review and fixes | ✅ | `f8d62ab7`, `3f44308c`, `e049cb0e`..`92b1c444` |

### Phase 1 · Window — running, 2 of 5 steps

**Step 1e · Chrome snapshot driver (test-only) — done, 7 of 7 tasks**

| Task | State | Commits |
|---|---|---|
| 1 · Shared QML mocks; Main.qml under all four chrome rows | ✅ | `978f51a9` |
| 2 · The snapshot plan as data | ✅ | `5bd02d75` |
| 3 · The offscreen scene for each script | ✅ | `b3486207`, `cc90f0e9` |
| 4 · PNG and manifest writer | ✅ | `a7e42cfe` |
| 5 · The snapshot driver (96 images) | ✅ | `c410c94d` |
| 6 · CI step and `chrome-snapshots` artefact | ✅ | `ed2d00e0` |
| 7 · Finish: nothing ships, simplify, step review | ✅ | `299ed8e4`, `754bdcd0` |

**Step 1a · Kit primitives, tokens, accessibility — done, 12 of 12 tasks**

| Task | State | Commits |
|---|---|---|
| 1 · Token groups and accessibility preferences as data | ✅ | `4e99e15e`, `8c84149e` |
| 2 · macOS accessibility and appearance sources, null fallback | ✅ | `14a63374`, `e28e2219` |
| 3 · Token groups on `chromeStyle`, rebuilt when the OS changes | ✅ | `82e945f5`, `8c26c45f` |
| 4 · Size-literal lint with a shrinking baseline | ✅ | `8562dfef` |
| 5 · Banner | ✅ | `f3347023` |
| 6 · Toast | ✅ | `a85ec37d` |
| 7 · KitDialog | ✅ | `29e9fb29` |
| 8 · Hosting notice becomes a Banner | ✅ | `30c3c300` |
| 9 · Reduce Motion for the terminal's transitions | ✅ | `6b6b0b8d` |
| 10 · Chrome follows an OSC 11 background | ✅ | `12391591`, `63e7ea09` |
| 11 · One-time new-defaults offer | ✅ | `c2a4f551`, `050d7191` |
| 12 · Finish: simplify, 160 snapshots, legibility fix, release notes, step review | ✅ | `c868c202`, `56fbc1ea`, `83d59e8c`, `3a4e2402` |

**Step 1b · Menus as data, native menus, menu bar — running, 11 of 13 tasks done** (plan `e3b3983f`, 24 pre-flight fixes folded in `7a85cfee`)

| Task | State | Commits |
|---|---|---|
| 1 · Menu rows use enums, carry a role and a colour swatch | ✅ | `bb923ec2` |
| 2 · Close Other Tabs, Close Tabs to the Right, Move Tab to New Window, About | ✅ | `87257ebb`, `82e96e24` |
| 3 · Tab, new-tab and tab-bar menus as tables | ✅ | `d8bfba8d` |
| 4 · One menu tree; a shortcut shows on one row only | ✅ | `26e993ce` |
| 5 · Menu token, presenter interface with a null fallback, router | ✅ | `6b31897e` |
| 6 · Native macOS context menu (NSMenu) | ✅ | `7fc7b058`, `3b987890` |
| 7 · All four context menus through the router | ✅ | `4b4e2fb8`, `0dad4c9f` |
| 8 · In-window menus drawn from the tables, swatch row, tab-menu snapshot | ✅ | `656dc79d`; 176 snapshots |
| 9 · About dialog and its snapshot | ✅ | `f4dd24d1`; 192 snapshots |
| 10 · Menu bar table (six menus) and its rule tests | ✅ | `3ad2a923` |
| 11 · The terminal keeps every key except Quit and Settings | ✅ | `30464032`, `cbcbb084` (three extra tests: ⌘Q under the Ctrl/Meta swap, live ⌘Q left to the menu while keys reach the PTY, a bound action) |
| 12 · The menu bar itself; Window and Help menus handed to macOS | ▶ | |
| 13 · Finish: simplify, every filter, snapshots, release notes, step review | ○ | |

**Step 1c · Title band, tab well and pills, badges, host chip, tab colour, full screen** — ○ not planned yet

**Step 1d · macOS title-band material and safe area** — ○ not planned yet

### Later phases

| Step | State | Delivers |
|---|---|---|
| 4a | ○ | Settings window shell; the settings pseudo-tab removed |
| 2 | ○ | Pane cards, headers, gap and hit area, densities, pane states |
| 3a–3f | ○ | Status line, find bar, notices and permissions, command palette, overlays, command blocks |
| 4b | ○ | Settings sections and controls, one save model |
| 5a, 5b | ○ | Windows 11 (Mica, Snap Layouts, Fluent) and Linux (header bar, portals) |
| 6.1–6.10 | ○ | Moments, one PR each |

**Now:** Step 1b Task 12, the macOS menu bar. The branch is rebased onto master and pushed.
**Next:** Step 1b Task 13 (finish), then plan Step 4a. Before merge, on the owner's word: regroup the commits semantically, put fixes to pre-existing bugs first, and upstream any vendored changes. Start from the plans index's "Resume here".

## Key rulings

- 2026-10-24 15:08:23: plans are written per step, each just in time when the step before it lands, from the implementation design.
- 2026-10-24 15:09:46: the plans tell a continuing agent how to update this board (`/status-board`), but carry no URL, because the board is private and the plans are public.

- 2026-10-24 16:38:04: every build uses at most 3 parallel jobs (`-j 3`), owner instruction.
- 2026-10-24 16:38:04: the Settings label "Match terminal" waits for Step 4b's per-option labels; `terminal` is listed first until then.

- 2026-10-24 19:31:59: text on a status-coloured fill is black or white, whichever contrasts more; the palette's highlighted-text colour pairs only with the highlight.
- 2026-10-24 20:00:10: bundled fonts are registered once from the app's start-up code, which is handed the font directories, rather than inside the font-locator factory; the system font registry is process-wide anyway.
- 2026-10-24 21:02:34: GUI tests keep running on the classic defaults; the new defaults get GUI coverage with Step 1e's snapshot driver. The tab label's process-name fallback moves to Step 1c.
- 2026-10-24 21:49:00: an existing config keeps its old `theme` default (follow the OS); `theme: terminal` becomes a default only for new configs.
- 2026-10-24 22:18:58: formatting is checked with clang-format 22, the version CI uses; this Mac's default clang-format is 23 and formats a few constructs differently.
- 2026-10-24 22:28:28: Step 1e snapshots the full matrix, light and dark included (96 images), and does not fail CI, like the existing display tests.
- 2026-10-25 00:08:30: Step 1a: dialog placement is a design token, not another layout switch; the hosting banner has no Retry button yet; a config that says `defaults_edition: classic` never sees the new-defaults banner.
- 2026-10-25 03:49:47: Step 1b: macOS context menus are built as native NSMenus; the colour swatch row and Move Tab to New Window appear on every OS; About is an in-window dialog; the menu bar has the six menus the plan proposes.
- 2026-10-25 10:26:15: the disk filled during Step 1b Task 4; on the owner's word `out/appleclang-tracy` was deleted (2.6 GB), leaving 2.7 GB free.
- 2026-10-24 16:41:42: contour-light's idle status-line bar is light (#2B2E35 on #ECEDEA, as on the canvas); the plan's white-on-black would have been 2.34 : 1.

## Follow-ups

- Disk space is tight: 11 GB free at 2026-10-25 13:54:20. Another full disk would stop the build again.

- The ⌘Q cases of Task 11 run only on the real macOS platform with `CONTOUR_TEST_DISPLAY=1`; no CI lane runs them (deferred to Step 1b's final review).

- **Windows is unverified in CI.** master's own Windows build fails in `src/vthost/SshTransport.cpp` (MSVC errors at lines 210–223), so CI stops before compiling this branch's Windows-only code (the Win32 frame re-apply, the null font and appearance factories). Needs a fix on master, or a local Windows build.

- Two `[display]` cases fail on this Mac with a real display (`sizeIncrement`, the latency probe); they fail the same way at the base commit, so they are not from this work.

- master's own CI is red (clang-tidy in vendored glaze, latency trends, several platform jobs), so those checks fail on PR #2178 too and are not this branch's to fix.


- Full `vthost_test` stalled on macOS before the rebase. On master `974feaf1` it completes (10595 assertions pass, 2026-10-25 13:50).
- Configuring master needs `cargo` for the bundled extensions; this Mac builds with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`.
