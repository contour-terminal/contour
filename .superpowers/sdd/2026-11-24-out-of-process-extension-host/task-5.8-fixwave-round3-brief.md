# 5.8 fix wave — review round 3

Re-review of round 2 (head 790ce3259): items 1,2,3,5 ✅, item 4 ❌ Critical. Fold into owning commits.

1. **Critical — `cmake/ApplyPatchOnce.cmake:73-77` silently skips the patch on BSD patch (macOS/FreeBSD).** `patch -R --dry-run` on BSD asks "Unreversed patch detected! Ignore -R? [y]" and, without a tty, answers yes → script reports "already applied". Ruling: replace `patch` with `git apply` (`git apply --check --reverse` → already applied; else `git apply --check` → apply; else FATAL half-applied). Works outside a git repo; git is already required by CPM. Drop the patch.exe search. Prove on Linux the three states (unapplied, applied, half-applied) by a small script test; and run it under a non-tty (`</dev/null`) to show no prompt.
2. **Minor — `WasmtimeEngine.cpp:540-550`:** the `os error N)` markers are OS-specific. Gate them per platform (ENOMEM=12 POSIX only; Windows: ERROR_NOT_ENOUGH_MEMORY 8, ERROR_OUTOFMEMORY 14 under `_WIN32` only) so EFAULT/ENOEXEC/ERROR_INVALID_ACCESS stay RuntimeUnavailable. Extend the classifier unit test per platform.
3. **Minor — wiring untested:** if a cheap seam exists, add one test that a Wasmtime priming error carrying the OOM text reaches `OutOfMemory` through compilePriming/runPriming; otherwise state the gap in the report (it goes in the PR's risk section).

Ruling on item 5 follow-up: CMake dependency rows and fetch messages ("Using CPM to fetch WAMR") are developer build output naming a third-party dependency, like libunicode — not product text. Keep them.

Verify: ASan targeted vtextension suites, the patch-state test, clang-tidy diff, lints, clang-format 22. Report: task-5.8-fixwave-round3-report.md.
