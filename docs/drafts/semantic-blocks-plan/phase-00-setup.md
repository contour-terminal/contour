# Phase 0 — Worktree build baseline

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A configured, building debug tree in this worktree and a recorded baseline of the test suite, so every later phase can tell its own failures from pre-existing ones.

**Architecture:** No source changes. Configure the `clangcl-debug` preset inside the worktree, build the test targets, run the suite, and write the baseline into the phase-0 commit message.

**Tech Stack:** CMake presets, Ninja, MSVC (`cl.exe` behind the `clangcl-*` presets), ctest.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §15 (delivery order). Global constraints: [README](README.md#global-constraints).

---

### Task 0.1: Configure and build the worktree tree

**Files:** none (build tree `out/build/clangcl-debug/` is git-ignored).

**Interfaces:**
- Consumes: nothing.
- Produces: `out/build/clangcl-debug/bin/*_test.exe` used by every later phase.

- [ ] **Step 1: Confirm the branch and a clean tree**

Run: `git status --short` and `git branch --show-current`
Expected: no output from the first (git-excluded local files do not show); `feature/1010-worktree-semantic-blocks` from the second (the plan and preflight.md are committed before phase 0 starts).

- [ ] **Step 2: Configure**

Run: `cmake --preset clangcl-debug` (Linux: `cmake --preset clang-asan`; macOS: `cmake --preset appleclang-debug` — see README "On Linux or macOS" for the rest of the translation)
Expected: ends with `-- Build files have been written to: .../out/build/clangcl-debug`. If vcpkg/Qt are not found, compare with the main tree's `D:\contour\out\build\clangcl-debug\CMakeCache.txt` (`Qt6_DIR`, `CMAKE_TOOLCHAIN_FILE`) and pass the same values with `-D`.

- [ ] **Step 3: Build everything the tests need**

Run: `cmake --build --preset clangcl-debug`
Expected: build succeeds with zero warnings. If it fails in `src/contour/display/*` on `yaml-cpp/emitter.h`, that break is pre-existing on this machine (memory: windows-verify-tricks); build the test targets individually instead:
`cmake --build --preset clangcl-debug --target vtbackend_test vtparser_test vtrasterizer_test vthost_test vtworkspace_test contour_test contour_gui_test vtconformance_test vtpty_test`
and record which targets could not be built.

### Task 0.2: Record the baseline

**Files:** none.

- [ ] **Step 1: Run the suite**

Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
Expected: note the `N tests passed, M failed` line. Known environmental failures (e.g. `vtconformance_vttest` without a `vttest -c` binary, `contour_gui_test` without staged Qt DLLs — see README) are recorded, not fixed.

- [ ] **Step 2: Run the spelling check**

Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS (or SKIP when `typos` is not installed; record which).

- [ ] **Step 3: Report the baseline**

Report to the coordinating session: the ctest summary line, the names of every failing test, the
spelling-check result, and any target that could not be built. The coordinator records it. No
commit — an empty commit would only add noise to the PR's history.

Phase gate: none (no code). Phase 1 starts from the current `HEAD`.
