# Contour — status

The status board's source, committed with the branch so another agent can resume; rendered and published to the board linked from PR #2178. Times are local, from `date`, to the second.

**Last updated:** 2026-10-25 12:14:59

## Current plan: a native look for Contour on macOS, Windows and Linux

The redesign gives Contour native chrome on every desktop: a title band with tabs, pane cards, a real menu bar, a Settings window and a useful status line. It keeps one component tree and a first-class TUI style. The design and the engineering plan are in draft PR #2178. Work runs step by step on `feature/native-redesign-phase0`, one PR-sized step at a time. Phase 0, the foundations, is finished; Phase 1, the window chrome, has its snapshot driver and kit primitives, and menus as data (Step 1b) are under way.

**Decisions:**
- 2026-10-25 12:14:59: work paused on the owner's word.
- 2026-10-25 12:08:32: on the owner's word, this board's link is in PR #2178 and in every plan, and the run's ledgers are committed under `docs/superpowers/ledgers/` (`59726671`, brought up to date in `c1a6cd8c`), so another agent can resume and keep the board current.
- 2026-10-25 03:15:52: Step 1a finished and pushed (`d6f91bd6`).
- 2026-10-24 23:47:03: Step 1e finished and pushed (`d210ae10`).
- 2026-10-24 22:13:13: Phase 0 finished and pushed (`9048ee2e`).
- 2026-10-24 14:35:23: TUI style on macOS keeps AppKit's real traffic lights; status line on by default; all eight proposed defaults approved (recorded in `8b706d62`).
- 2026-10-24 13:04:06: PR #2176 merged; the redesign branch was rebased onto `4637e4a6`.
- 2026-10-24 14:35:23: each task runs only its own tests; the full suite runs in CI (first applied to `1129e623`).
- 2026-10-24 15:08:23: the implementation design passed three Fable review rounds and was committed (`d06d03a3`).
- 2026-10-24 15:09:15: draft PR #2178 opened.
- 2026-10-24 15:11:47: plans index and the reviewed Phase 0 plan committed (`bd8ffbda`) and pushed.

| Phase | Steps done | State | Where it is |
|---|---|---|---|
| 0 · Foundations | 1/1 | done | 9 of 9 tasks, plus a whole-branch review; pushed to PR #2178 |
| 1 · Window: 1e snapshot, 1a kit, 1b menus, 1c title band, 1d material | 2/5 | on hold — paused by the owner | 1e and 1a done; 1b at 11 of 13 tasks; 1c and 1d not yet planned |
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
| 1 · Chrome style rows (MacOS, Fluent, ContourLinux, Terminal) | ✅ | `1129e623`, `de93f290` |
| 2 · macOS shortcut glyphs ⌘⌥⇧⌃ | ✅ | `5dc78c1e` |
| 3 · `theme: terminal`: chrome follows the terminal's light or dark | ✅ | `2beb339f`, `fbc14e8e` |
| 4 · contour-dark, contour-light and contour-classic schemes | ✅ | `b01884e6`, `14fd4463`, `32700ca5` |
| 5 · No focus frame around a single pane | ✅ | `f502d1d5`, `acad1fa1` |
| 6 · No colour literals in chrome QML; status colour tokens | ✅ | `85ef262b`, `f2bf77ae`, `20799948` |
| 7 · Bundled JetBrains Mono | ✅ | `4c12a09e`, `907412ee` |
| 8 · First-launch defaults behind `defaults_edition` | ✅ | `393e050c`, `064576f4` |
| 9 · Simplify, release notes, whole-branch review and fixes | ✅ | `fd34db81`, `eb78bcc5`, `ef91d7a2`..`9048ee2e` |

### Phase 1 · Window — running, 2 of 5 steps

**Step 1e · Chrome snapshot driver (test-only) — done, 7 of 7 tasks**

| Task | State | Commits |
|---|---|---|
| 1 · Shared QML mocks; Main.qml under all four chrome rows | ✅ | `bb1e394d` |
| 2 · The snapshot plan as data | ✅ | `a7a4160f` |
| 3 · The offscreen scene for each script | ✅ | `2c3cf05e`, `74729c45` |
| 4 · PNG and manifest writer | ✅ | `b1fd88e3` |
| 5 · The snapshot driver (96 images) | ✅ | `9a3b36ac` |
| 6 · CI step and `chrome-snapshots` artefact | ✅ | `378a3fab` |
| 7 · Finish: nothing ships, simplify, step review | ✅ | `034368b1`, `c0d823f9` |

**Step 1a · Kit primitives, tokens, accessibility — done, 12 of 12 tasks**

| Task | State | Commits |
|---|---|---|
| 1 · Token groups and accessibility preferences as data | ✅ | `1c8fe545`, `58f11960` |
| 2 · macOS accessibility and appearance sources, null fallback | ✅ | `ad3dcd48`, `a555ee72` |
| 3 · Token groups on `chromeStyle`, rebuilt when the OS changes | ✅ | `1d4f64e5`, `2a204048` |
| 4 · Size-literal lint with a shrinking baseline | ✅ | `f5aae0aa` |
| 5 · Banner | ✅ | `ddca983a` |
| 6 · Toast | ✅ | `ff74857b` |
| 7 · KitDialog | ✅ | `2a009bed` |
| 8 · Hosting notice becomes a Banner | ✅ | `d7535561` |
| 9 · Reduce Motion for the terminal's transitions | ✅ | `480419b9` |
| 10 · Chrome follows an OSC 11 background | ✅ | `06e0a550`, `f786d35a` |
| 11 · One-time new-defaults offer | ✅ | `8167cb11`, `200d761d` |
| 12 · Finish: simplify, 160 snapshots, legibility fix, release notes, step review | ✅ | `ddf32200`, `159b47dc`, `89c21127`, `d6f91bd6` |

**Step 1b · Menus as data, native menus, menu bar — paused, 11 of 13 tasks** (plan `96baa447`, 24 pre-flight fixes folded in `d7f2f4f8`)

| Task | State | Commits |
|---|---|---|
| 1 · Menu rows use enums, carry a role and a colour swatch | ✅ | `0eb3e889` |
| 2 · Close Other Tabs, Close Tabs to the Right, Move Tab to New Window, About | ✅ | `2aa4a6f1`, `3b6e1dce` |
| 3 · Tab, new-tab and tab-bar menus as tables | ✅ | `8f7a3069` |
| 4 · One menu tree; a shortcut shows on one row only | ✅ | `fb582412` |
| 5 · Menu token, presenter interface with a null fallback, router | ✅ | `ec0977eb` |
| 6 · Native macOS context menu (NSMenu) | ✅ | `39f2bf14`, `841b13b0` |
| 7 · All four context menus through the router | ✅ | `807a820e`, `50a3bf1c` |
| 8 · In-window menus drawn from the tables, swatch row, tab-menu snapshot | ✅ | `0dff3a88`; 176 snapshots |
| 9 · About dialog and its snapshot | ✅ | `18a73c7f`; 192 snapshots |
| 10 · Menu bar table (six menus) and its rule tests | ✅ | `f3ea8308` |
| 11 · The terminal keeps every key except Quit and Settings | ✅ | `0952516f` approved; a follow-up round of three extra tests was interrupted, its partial edits left uncommitted |
| 12 · The menu bar itself; Window and Help menus handed to macOS | ○ | |
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

**Now:** paused by the owner. Everything is committed and pushed (`c1a6cd8c` and the pause note after it); no agent is running.
**Next:** on resuming, finish Task 11's three extra tests (partial edits in `DisplayRendering_test.cpp` and `ShortcutOverride_test.cpp`), then Step 1b Tasks 12–13, then plan Step 4a. Start from the plans index's "Resume here".

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

- Disk space is tight: 2.1 GB free at 2026-10-25 11:17:02, after deleting the tracy build tree; `out/appleclang-debug` is now 18 GB. Another full disk would stop the build again.

- **Windows is unverified in CI.** master's own Windows build fails in `src/vthost/SshTransport.cpp` (MSVC errors at lines 210–223), so CI stops before compiling this branch's Windows-only code (the Win32 frame re-apply, the null font and appearance factories). Needs a fix on master, or a local Windows build.

- Two `[display]` cases fail on this Mac with a real display (`sizeIncrement`, the latency probe); they fail the same way at the base commit, so they are not from this work.

- master's own CI is red (clang-tidy in vendored glaze, latency trends, several platform jobs), so those checks fail on PR #2178 too and are not this branch's to fix.


- Full `vthost_test` stalls on macOS (a full run sat for about 10 minutes with little CPU use). Likely in PR #2176's daemon tests; not investigated.
- Configuring master needs `cargo` for the bundled extensions; this Mac builds with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`.
