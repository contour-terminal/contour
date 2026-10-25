# SDD ledger — plan: docs/superpowers/plans/2026-10-23-native-redesign-phase0.md

Spec: docs/superpowers/specs/2026-10-23-native-redesign-design.md (reachable)
Branch: feature/native-redesign-phase0 (from master 9f392470). Build tree: out/appleclang-debug (macOS, Qt 6.11).
Task 0: complete (commit f75c0b25; prototype stashed as "macos-command-key-bindings prototype (see #2175)")

## Pre-flight scan

| Pair / task | Produces → consumes | Finding |
|---|---|---|
| T1 ↔ T6 | T1: ChromeStyle rows + UiStyleProvider(ChromeStyle,…) → T6 adds statusColors token to every row + statusColor() on provider | consistent; T6 must fill all 4 rows |
| T3 ↔ T4 | T3: relativeLuminance (Color.hpp) → T4 contrastRatio built on it | consistent (order fixed in plan) |
| T3 ↔ T6 | T3: colorSchemeOfBackground → T6 provider picks light/dark status colors | consistent |
| T3 ↔ T8 | T3 default theme Terminal; T8 default colors Dual(contour-light/dark) → Terminal+Dual = follow OS | consistent, intended |
| T4 ↔ T8 | scheme names contour-dark/light → T8 defaults | consistent |
| T5 ↔ T6 | T5 removes #3daee9, frame uses palette.highlight → T6 lint must pass | consistent (order fixed) |
| T7 ↔ T8 | T7 registers JetBrains Mono → T8 default family | consistent |
| T1, T3, T8 share Config.hpp | sequential edits, different sections | no conflict |
| T1 self | tests vs table: native row values 34/32/3/Fusion | consistent with UiStyle.hpp today |
| T2 self | glyph order ⌃⌥⇧⌘ = table order for those rows | consistent |
| T3 self | `constexpr` signatures vs relativeLuminance needing pow() | CONFLICT → ruling R1 |
| T4 self | test API `defaultColorPalettes(name)` shape unknown | plan says adapt shape not assertions; ok |
| T5 self | PaneNode inSplit → TerminalPane showFocusFrame | consistent |
| T6 self | lint flags any `Qt.rgba(` → but chrome legitimately uses Qt.rgba(palette.x.r, …, alpha) | CONFLICT → ruling R2 |
| T7 self | new asset files + spelling/lint gates | ok; gates must accept |
| T8 self | bell `.sound = "off"` — silent spelling unverified | ambiguity → ruling R3 |

## Rulings
- Ruling R1: drop `constexpr` from `colorSchemeOfBackground` / `qtColorSchemeFor` / `relativeLuminance` (plain `[[nodiscard]]` functions) — WCAG luminance needs `std::pow`, not constexpr before C++26 — cost if wrong: none functionally.
- Ruling R2: the QML color lint flags hex literals and `Qt.rgba(` only when all four arguments are numeric literals; `Qt.rgba(color.r, color.g, color.b, a)` derivations are allowed — spec forbids literal colors, not alpha derivations of palette colors — cost if wrong: a few numeric-literal colors slip through.
- Ruling R3: for a silent default bell, use the value the existing Bell implementation already treats as "no sound" (check BellSound/DeferredBell and docs); if none exists, use `volume = 0.0` with `sound = "default"` rather than inventing a new spelling — cost if wrong: generated config shows an unexpected spelling.

## Tasks
Task 1: dispatched (BASE f75c0b25, implementer a4993080672ef265a)
Task 1: PAUSED by user before review (implementer stopped; working tree may hold partial Task 1 edits — inspect git status before resuming)
Task 1: resumed by user (same implementer a4993080672ef265a)
Task 1: committed 1129e623 after rebase onto master 4637e4a6 (PR #2176); tests run: contour_gui_test + contour_test UiStyle/ContourTui filters PASS. Task review NOT yet run.
Note: branch rebased onto 4637e4a6; 84a597ff (vthost SOCK_CLOEXEC macOS fix) precedes Task 1. Plan revised in bd8ffbda (status board section, DefaultsEdition gate in Task 8, blockStatus Running fallback in Task 4, hover-tooltip default moved to step 3e, existing relativeLuminance/contrastRatio consumed).
- Ruling R1 (revised): relativeLuminance/contrastRatio already exist (Color.hpp:109/:116) — Tasks 3/4 consume them; colorSchemeOfBackground is a plain [[nodiscard]] function — cost if wrong: none.
- Ruling R4: per owner, each task runs only its own test filter; full suite runs in CI; local configure uses -DCONTOUR_EXTENSIONS_BUNDLED=OFF (no cargo) — cost if wrong: a cross-module regression surfaces in CI rather than locally.
Task 1: review dispatched (sonnet, BASE 84a597ff HEAD 1129e623)
Task 2: dispatched (BASE bd8ffbda, implementer ad2d6c8e7573dbcb8, sonnet)
Task 1: review — spec ✅, quality Approved with 1 Important (ContourGuiApp.hpp ~:88 _chromeStyle inserted between the lifetime comment and _uiStyleProvider). Fix round 1 queued until Task 2 implementer finishes (no parallel implementers).
Task 1: ⚠️ to resolve by controller: run MainWindow* and [chrome] filters (geometry); chromeStyleName() mapping untested; UiStyle.hpp now includes WindowControlStyle.hpp (check no Qt/cycle).
Task 1: minor (deferred): UiStyle.hpp NativeTokens/TerminalTokens indentation (clang-format), stale "selecting Native" comment, ChromeStyleCount hand-maintained.
Task 1: minor (deferred): UiStyle_test naming style; QmlChromeStyle installChromeStyle takes UiStyle (needs ChromeStyle overload in later phases); _chromeStyle default ContourLinux before resolve.
Task 2: implementer DONE_WITH_CONCERNS (commit 5dc78c1e): RED step skipped; ShortcutStyle also threaded into shortcutIndex/describeLinkGesture; macOS link gesture reads "⌃Click".
- Ruling R5: macOS link-follow modifier should be ⌘ per the design (moments 6); that is a default, handled in Task 8 (Native2026 table), not Task 2 — cost if wrong: macOS shows ⌃Click until Task 8.
Task 1: ⚠️ resolved: contour_gui_test "[mainwindow]" PASS (127 assertions, 9 cases) — geometry unchanged. chromeStyleName untested → minor (deferred). Include of WindowControlStyle.hpp compiled in Qt-free contour_config → no Qt dependency.
Task 1: fix round 1/5 dispatched (haiku fixer a570a43316c7452ce, FIX_BASE 5dc78c1e)
Task 2: review dispatched (sonnet acf9ac7ca5b9b2049, BASE bd8ffbda HEAD 5dc78c1e)
Task 2: review — spec ✅ (deviation: style threaded into shortcutIndex/describeLinkGesture, tests pass Text — judged an improvement), quality Approved.
Task 2: minor (deferred): clang-format over-limit lines QmlComponents_test.cpp:3784, CommandPaletteModel_test.cpp:~329; stale @copydoc shortcutIndex(config::InputMappings const&); "⌃Click" wording; no mixed glyph/text chord test; no platformShortcutStyle() test; RED step skipped.
Task 2: complete (commits bd8ffbda..5dc78c1e, review clean)
Task 3: dispatched (BASE de93f290)
Task 1: fix round 1 committed de93f290; scoped re-review dispatched (haiku aab96e3a3cb3a2788)
Task 3: implementer a50b38854e5b283b4 (sonnet)
Task 1: fix round 1/5 (1 addressed, 0 open; commits 5dc78c1e..de93f290)
Task 1: complete (commits 84a597ff..1129e623 + de93f290, review clean)
- Ruling R6: owner instruction — every build uses at most 3 parallel jobs (cmake --build ... -j 3); carried in every implementer/fixer dispatch — cost if wrong: slower builds only.
Task 3: implementer DONE_WITH_CONCERNS (commit 2beb339f): no per-option label "Match terminal"; colorSchemeChanged handler now gates on _guiPaletteOverridden; WindowController native-frame lambda updated; colours from default profile.
- Ruling R7: the "Match terminal" combo label needs per-option labels in globalEnumField; deferred to Step 4b (Settings controls), `terminal` listed first now — cost if wrong: Settings shows the raw word "terminal" until 4b.
Task 3: review dispatched (sonnet, BASE de93f290 HEAD 2beb339f)
Task 4: dispatched (BASE 2beb339f)
Task 4: implementer a3956339f5511a337 (sonnet)
Task 3: review — spec ✅, quality Approved; ⚠️ reload path: applyGuiTheme has only startup + settings-apply callers (pre-existing), so with theme: terminal a hand-edited contour.yml colour change live-reloads the grid but not the chrome.
- Ruling R8: that ⚠️ is a real gap for the new default (spec: chrome follows the terminal background; impl design §2.2 assumed a reload path that does not exist) — Task 3 fix round 1: call applyGuiTheme(theme, defaultProfileColors()) after a successful live config reload (TerminalSessionManager reload path), with a test if the path is headless-testable, plus fix the two stale comments ("tracked only in System mode", "gated to System mode regardless"). Runtime OSC 11 background changes: parked to Step 1a (chrome follows the session's live background) — cost if wrong: chrome stale after OSC 11 until then.
Task 3: minor (deferred): unrelated clang-format churn in ContourGuiApp.cpp/Config_test.cpp; no test for the handler gate (non-headless run()); defaultProfileColors() assumes the default profile exists.
Task 3: fix round 1 queued until Task 4 implementer finishes.
Task 4: implementer DONE_WITH_CONCERNS (commit b01884e6): light idle indicator pair 2.34:1 per plan → used scheme bg as text via new idleIndicatorText; selection alpha 1.0; edited ConfigDocumentation.hpp (Running fallback blue #4D79FF), Config_test not run.
- Ruling R9: contour-light idle (Inactive/Insert) status-line pair follows the canvas status-line board: fg #2B2E35, bg #ECEDEA (≈12:1), a light bar one step off the background; dark keeps fg palette white / bg palette black. The plan's "white on black" for light was a plan defect (2.34:1 violates the 4.5:1 rule) — cost if wrong: one colour pair to revisit.
- Ruling R10: selection alpha 1.0 accepted — spec gives an opaque selection where text keeps its colour.
Task 4: review dispatched (sonnet, BASE 2beb339f HEAD b01884e6); Task 3 fix round 1 dispatched (FIX_BASE b01884e6)
Task 4: reviewer abfd0fb0d0c6b5ef4; Task 3 fixer ae36b3118a7b32680
Task 4: review — spec ✅ (all values + contrasts verified independently), quality Approved. ⚠️ Config_test not built/run (doc text #4D79FF looks right).
Task 4: fix round 1 queued (after Task 3 fixer): apply R9 (light idle pair fg #2B2E35 / bg #ECEDEA — rework idleIndicatorText into an explicit idle pair in ContourScheme data, test reads it from data, no name branch); fold minors ConfigDocumentation.hpp:1832 "normal color 3"→4, ColorPalette.hpp:392 >110 cols; build + run Config_test to resolve ⚠️.
Task 3: fix round 1 committed fbc14e8e (reload re-applies theme; settings-apply call removed as redundant; [theme] reload test); scoped re-review dispatched. Task 4: fix round 1 dispatched (FIX_BASE fbc14e8e)
Task 3: fix round 1/5 (2 addressed, 0 open; settings path verified safe — every reload caller goes through reloadAllSessions; commits b01884e6..fbc14e8e)
Task 3: complete (commits de93f290..2beb339f + fbc14e8e, review clean)
Task 3: minor (deferred): TerminalSession.hpp:1380 bool reloadConfig(...) is a bool-return API (pre-existing, out of scope).
Task 4: fix round 1 committed 14fd4463 (R9 light idle pair as data, doc fixes, Config_test run — found+fixed stale block_status_hover running #B8B868→#8EA4E7); scoped re-review dispatched.
Task 5: dispatched (BASE 14fd4463)
Task 4: re-reviewer aa179cd5962a98f04; Task 5: implementer a81c78d7c6a44646e
Task 4: fix round 1/5 (4 addressed; NEW Important breakage: fix round deleted coverage — contourSchemesAreReadable lost the 16-colour + fg/bg contrast and exact-value assertions, cursorAndSelection test removed; commits fbc14e8e..14fd4463)
Task 4: fix round 2 queued after Task 5 implementer (3-job build limit forbids concurrent builds): restore the readability assertions (fg/bg ≥ 4.5, colours 1–6 and 9–14 ≥ 4.5, exact bg/palette checks from b01884e6) and the cursorAndSelection test from b01884e6, keeping the new IdlePairs indicator test as its own TEST_CASE.
- Ruling R11: never run two builds at once (owner's 3-thread limit is a total, not per agent) — implementers and fixers that build are strictly sequential — cost if wrong: slower wall-clock only.
Task 5: implementer DONE (commit f502d1d5); review dispatched. Task 4: fix round 2 dispatched (resume fixer a9714bcac65814df1, FIX_BASE f502d1d5)
Task 5: review — spec mostly ✅; Important: stray sizeWidget edit in TerminalPane.qml (border.width 1 → paneActive ? 1 : 0, ~former :150), out of scope, regresses the resize popup's border in inactive panes. Nested-split recursion and zoom verified by reading.
Task 5: minor (deferred): no zoom or nested-split test; redundant border.width on the frame; test placed after the .moc include.
Task 5: fix round 1 queued after Task 4 fix round 2 (resume implementer a81c78d7c6a44646e).
- Task 4: fix round 2 committed 32700ca5 (restored contourSchemesAreReadable body and cursorAndSelection; IdlePairs kept as own TEST_CASE). Scoped re-review next.
- Task 4: scoped re-review of f502d1d5..32700ca5 dispatched (no build). Task 5: fix round 1 dispatched (resumed implementer; sole build).
- Task 4: re-review Approved (no findings). Task 4: complete (b01884e6, 14fd4463, 32700ca5).
- Task 5: fix round 1 committed acad1fa1 (sizeWidget border.width restored to 1). Re-review done inline by controller: 1-line revert; net diff vs master shows only the focus-frame changes. Approved.
- Task 5: complete (f502d1d5, acad1fa1).
- Task 6: BASE acad1fa1; implementer dispatched (sonnet), R2 carried in dispatch.
- Task 6: implementer DONE_WITH_CONCERNS, 85ef262b (lint 8/8; gui 4 cases; [qml] 118 pass). Concerns observational (test-name globs, brightText→highlightedText, allowlist = Main.qml #00000000). Review dispatched.
- Task 6: review Spec ✅, Quality Needs fixes (1 Important: danger-button text contrast 2.7:1 in dark; Minors: scrim label colour, WindowControls hover text, bare role ints, repeated call). Fix round 1 dispatched (resumed implementer).
- Ruling R12: on-colour text is chosen by contrast (black/white, higher ratio) via provider onStatusColor(role) — palette highlightedText pairs only with Highlight — cost if wrong: a later token rework in 1a.
- Parked (final review): lint ignores 3-digit hex / named colours / Qt.color("…") (acceptable under R2).
- Task 6: fix round 1 committed f2bf77ae (onColor/onStatusColor, paneDimTextColor, StatusRole Q_ENUM, hoisted danger). lint 8/8, UiStyleProvider 5 cases, [qml] 118. Scoped re-review dispatched.
- Task 6: re-review of f2bf77ae Approved (minors: static_assert misses new enumerators; no proof chromeStyle.Danger resolves in QML). Controller: the second is load-bearing (silent transparent fallback) → fix round 2 dispatched.
- Task 6: fix round 2 committed 20799948 — confirmed chromeStyle.<Enum> was undefined on the context-property instance (dialogs got transparent status colours); replaced by CONSTANT int properties statusDanger…statusRunning; QQmlEngine test proves resolution; count static_asserts + QMetaEnum key-count test. Re-review inline by controller (72-line diff; no instance-enum uses left). Approved.
- Task 6: complete (85ef262b, f2bf77ae, 20799948).
- Task 7: BASE 20799948.
- Task 7: implementer dispatched (sonnet).
- Task 7: implementer DONE_WITH_CONCERNS 4c12a09e (FontLocator 3 cases; lint 8/8; [qml] 119). Concerns: free registerBundledFonts not via factory; FontconfigLocator now uses current config (Linux untested here); font-locator verb doesn't register. Review dispatched.
- Task 7: review Spec ✅, Quality Needs fixes (Important: font-locator verb doesn't register; test fails where JetBrains Mono is system-installed. Minor: RegistrationFailed logging; undocumented deviation). fontconfig change judged sound. Fix round 1 dispatched (resumed implementer).
- Ruling R13: bundled-font registration is a free function taking the injected directory list, called from the composition root, not a FontLocatorProvider factory parameter — the target is the process-wide OS registry and the provider is a call_once singleton, so the factory route needs a static configure call (worse under AGENT.md) — cost if wrong: a small refactor when the provider stops being a singleton.
- Parked (final review): fontconfig installedCoverage would miss bundled fonts if ever built before registration (not the case with today's startup order).
- Task 7: fix round 1 committed 907412ee (shared registerBundledFonts() helper in both actions; test asserts any returned path is bundled; RegistrationFailed at error level by name; deviation documented). Re-review inline by controller (48-line diff). Approved.
- Parked (final review): font-locator helper borrows a temporary QCoreApplication only to read applicationDirPath(); a Qt-free executable-path lookup would be cleaner.
- Task 7: complete (4c12a09e, 907412ee).
- Task 8: BASE 907412ee.
- Task 8: implementer dispatched (opus; R3, R4, R5 carried; R4 overrides brief's full-suite steps).
- Task 8: implementer DONE_WITH_CONCERNS 393e050c (firstLaunchDefaults 310 assertions; [config] 303+31; [settings] 102; 262 default-sensitive cases; [qml] 119). Review dispatched (opus).
- Ruling R14: GUI test fixture TestApp builds Classic so behaviour tests keep their inputs; Native2026 GUI coverage arrives with Step 1e's snapshot driver — cost if wrong: a Native2026-only GUI regression is caught later.
- Ruling R15: the TabLabel foreground-process fallback moves to Step 1c (tab pills), which owns resolvedTabLabel — cost if wrong: empty-title tabs show the bare title until 1c.
- Task 8: review Spec ✅, Quality Approved (Classic table matches old defaults one by one; pre-scan, hoist, exclusions, reload all verified). Minor fix round dispatched: fallback profiles take the config's edition (ConfigProfileCatalog, SettingsController::newProfile), doc wording.
- Parked (Step 1a): ResetConfig writes native2026 to contour.yml but a settings.yml stating classic still wins — handle with the new-defaults banner.
- Parked (final review): WindowController pre-seed tab bar `Always` (overwritten by config seed).
- Task 8: fix round 1 committed 064576f4 (fallback profiles take the config's edition; new [settings] test; doc wording). Re-review inline by controller (23-line diff; local builtIn outlives its only use in resolve()). Approved.
- Task 8: complete (393e050c, 064576f4).
- Task 9: BASE 064576f4.
- Ruling R16: Task 9 Step 3's full ctest is replaced by CI (R4); Step 4's draft PR already exists (#2178) — push instead. Order: /simplify → release notes → final whole-branch review (most capable model) → push.
- Task 9: /simplify pass dispatched (opus) over 4637e4a6..064576f4, with parked cheap minors.
- Task 9: /simplify committed fd34db81 (14 files; parked cheap minors fixed; StatusRole mirror removed; currentExecutablePath replaces temp QCoreApplication; allowlist now empty). Focused suites pass. Controller ran vthost_test "[afunix],[attach],[acceptor]" for the setCloseOnExec error-path change: 56 cases pass. Running→blue confirmed intended (Task 4).
- Task 9: release notes committed eb78bcc5 (metainfo.xml 0.9.0; xmllint ok). Final whole-branch review dispatched (fable) on 4637e4a6..eb78bcc5.
- Final review (fable): Ready to merge: With fixes. Critical: Windows "missing directory" font test fails (no-op returns success). Important: `theme` default not edition-gated (Classic partial config gets Terminal); release-note overclaim "menus". Minor: statusColor bindings stale after live theme flip; "every color readable" overclaim; doc note on built-in palettes. Upgrade safety, cross-task, platforms, layering verified clean.
- Final fix dispatch (one, opus) covers all six.
- Ruling R17: Classic edition's theme is the pre-redesign default (System); Terminal moves to the Native2026 column — §2.7 says every visible default is gated — cost if wrong: none for new configs; Classic users keep OS-following chrome.
- Final fixes: ef91d7a2 (Windows DirectoryMissing), 159ca6c8 (theme edition column + doc note), 23887f90 (release notes), 9048ee2e (paletteRevision NOTIFY; QML uses the existing `rev < 0 ? … : …` dependency idiom). Focused tests pass. Scoped re-review inline by controller (Windows body uses the shared listBundledFontFiles; Classic theme = System). Residuals: none load-bearing.
- Task 9: complete. Phase 0 code complete; pushed bd8ffbda..9048ee2e to PR #2178 for CI (Linux/macOS/Windows).
- CI on 9048ee2e: Check C++ style failed (Config_test.cpp:6415 under clang-format 22; local is 23) and editorconfig failed (OFL.txt trailing space). Fixed in 715fd1a7, pushed. Ruling R18: format with /opt/homebrew/opt/llvm@22/bin/clang-format (CI's version) in every brief.
- Step 1e plan committed 552e5edf (7 tasks); board-file wording fixed in tracked plans 05fa207a; pushed. Ruling R19: Step 1e keeps the full snapshot matrix (incl. light/dark — Phase 0 made the chrome follow light/dark) and is non-gating in CI like the existing display tests — cost if wrong: 96 PNGs per CI run; easy to halve later.
- CI on 05fa207a: Linux (no GUI / no extensions) failed: Config.hpp:1247 missing designated field 'extension' under clang 22 -Wmissing-designated-field-initializers (appleclang silent). Fixed in 3635823a; all 23 changed TUs syntax-checked with llvm@22 clang. clang-tidy failure is in vendored glaze (vtcontrol/Describe.cpp) — adjacent, not this branch; verify against master. Windows latency trend failure: not yet read.
- Adjacent (pre-existing on master ef4e307e, run 40091161826): Check clang-tidy, Latency trend (windows-2022, macos-15), and many platform jobs fail on master itself. Not this branch's to fix; compare per-job against master before acting.
- Step 1a plan committed 6da3e0a0 (12 tasks), pushed. Ruling R20: Step 1a's three open choices taken as the plan proposes — dialog placement is a token (no sixth layout switch); no Retry on the hosting banner yet; an explicit defaults_edition: classic never shows the new-defaults banner — cost if wrong: small QML/token change later.
- Step 1b plan committed 96baa447 (13 tasks), pushed. Ruling R21: Step 1b's four open choices taken as the plan proposes — macOS context menus built as NSMenu directly (QMenu::popup is not documented native); the swatch row and Move Tab to New Window on every OS, as the spec's tab-menu table lists; About as an in-scene KitDialog; the six-menu menu bar as proposed (no canvas board exists) — cost if wrong: a table edit, or swapping the presenter's build step.
- CI on d6f91bd6/96baa447: Windows Build fails; master's Windows (run 40091161826) fails in Build with MSVC errors in src/vthost/SshTransport.cpp:210-223 (pre-existing, not this branch). Consequence: our Windows-only code (Win32 frame re-apply, null factories, Cascadia default) is not compiled by CI until master is fixed. PR log to be confirmed when run completes.
