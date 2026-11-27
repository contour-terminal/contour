# 5.8 fix wave — review round 1

Review: task-5.8-fixwave-review.md (same directory). Base: 75eb4d4a7. Fold each fix into the commit it belongs to (fixup + autosquash) where that rebases cleanly; otherwise a new commit. Every commit must still build on its own.

1. **Important — WAMR exec-env/cluster race (review Important 1).** First prove or disprove it against the WAMR sources we build (FetchContent copy under the build tree, `WAMR_BUILD_THREAD_MGR=1`): does destroying one instance's exec env unlink it from a list another instance's trap/exception path walks, without a lock the other side takes? Then:
   - real and fixable on our side (e.g. `WAMR_BUILD_THREAD_MGR=0` if we use no WAMR threads, or ordering under our process-wide lock without inversion): fix it, own `fix:` commit, with a `[threading]` test that makes calls and traps while another runtime unloads, failing under TSan without the fix;
   - real and only fixable in WAMR: a minimal patch applied by our FetchContent (PATCH_COMMAND) taking the cluster lock, same test;
   - not real: write why (file:line in WAMR) into the report; no code change.
2. **Drop item 10 (ruling):** revert/remove commit 75eb4d4a7 (`primingDone`) entirely — no measurable gain, extra interface and ownership change. Keep the measurement numbers in the report only.
3. `HostRefusals`: replace the hard-coded last step / unchecked shift with a compile-time bound on `lockdownSteps()` size (≤ 32 or use a wider mask) — static_assert, not a runtime assert.
4. `AppContainer_win32.cpp`: the remembered NotGranted directory suppresses only the second log line, never the check; a retry after an ACL fix must succeed. Extend the wine test.
5. Priming `OutOfMemory` maps to a transient refusal, not lasting; other priming codes stay lasting. Test.
6. The degraded-sandbox notice ("Extensions run … without …") logs as a warning, not an error.
7. Fix flaky `ExecutorThread.startReturnsOnceTheBodyRuns` (ExecutorThread_test.cpp:26): the assertion must not race the body. Run it 50× under TSan.
8. Formatting slips: `priming()` doc comment (gone with item 2 if it lived there), `_unavailable { 0 }` wrap in HostRefusals.hpp, long concurrency comment line in build.yml. Run /usr/bin/clang-format (v22) and editorconfig-checker.

Verify: ASan targeted suites (vtextension_*), TSan for 1 and 7, wine for 4, clang-tidy diff, lints. Report to task-5.8-fixwave-round1-report.md.
