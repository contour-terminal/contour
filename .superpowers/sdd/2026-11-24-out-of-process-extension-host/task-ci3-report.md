# CI fix 3: report

**Status:** DONE. Not pushed.
**Old head → new head:** `5798ba3f6` → `223c1aa25`. The branch was rebased with `--autosquash` onto `origin/master` (`8c3afaa0c`), which had not moved, so the base is the same. The new tree is byte-identical to the pre-rebase tip with the four commits on top (`git diff` empty), so the checks below hold for `223c1aa25`.

## 1. Windows (MSVC): LNK2005 duplicate `wasm_byte_vec_*` in `vtextension_remote_test`

**Root cause.** `vtextension_remote_test` linked Wasmtime (`vtextension_wasmtime`, `vtextension_wasmtime_hosted_engine`, `contour::wasmtime`). Through `if(TARGET vtextension_wamr_engine)` it also linked `vtextension_wamr_engine`, which brings in WAMR's `iwasm`, for `wamr/RemoteWamrEngine_test.cpp`. Both runtimes define the standard `wasm.h` C API.
- MSVC refuses the second definition.
- An ELF linker binds every caller to whichever definition it meets first, so Linux linked a binary in which one runtime could call the other's `wasm_byte_vec_new`.

The composition came in with `44f059428` ("the extension host can run its modules interpreted").

**Survey of all targets on all platforms.** This was checked by reading CMake and confirmed by the new lint. It is the only executable that can reach both runtimes:
- `vtextension_test`, `contour_test`, `contour_gui_test` and `vthost_test` link `vtextension_wamr` only.
- `vtextension_wasmtime_test` links Wasmtime only.
- `contour`, `bench-headless` and `contour-extension-host` choose one engine through a variable (`vtextension_${CONTOUR_WASM_RUNTIME}`, `${_extension_host_engine}`).
- `contour-extension-host-wamr` links WAMR only.
- `vtextension_helper_test`, `vtextension_wamr_helper_test` and `vtextension_process_test` link no engine.

**Fix (cleanest option, no coverage lost).** `RemoteWamrEngine_test.cpp` is now its own binary, `vtextension_remote_wamr_test` (`src/vtextension/wasm/remote/CMakeLists.txt`). It links `vtextension vtextension_remote vtextension_remote_wire vtextension_host vtextension_wamr_engine core::testing Catch2 contour_test_trace` and no Wasmtime. It is built wherever `vtextension_wamr_engine` exists, which is always under `VTEXTENSION_TESTING`, and no longer only where Wasmtime is. So the WAMR remote contract now also runs on builds without Wasmtime. No `/FORCE` and no suppression. Folded into `44f059428` → **`fb5b86dec`**, whose message gains a paragraph on why.

**Lint (new commit `223c1aa25`, `build: the containment gate refuses an executable that links two WebAssembly runtimes`).**
- `scripts/check-wasm-runtime-containment.py` gains rule 4. It reads the link graph from `add_executable`/`qt_add_executable`/`target_link_libraries` in every `CMakeLists.txt`/`*.cmake` under `src/` and `cmake/`, with comments stripped.
- Every condition is taken, so a pair that only one platform builds fails on every platform, Linux included.
- It reports each executable that reaches both `contour::wamr` and `contour::wasmtime` transitively.
- Library names that contain `$` (variables, generator expressions) are not followed: a variable picks one runtime, never both.
- The self-test grows from 31 to 38 cases: direct, transitive with a conditional link, single-engine, variable, comment, generator expression, and the reported `path:line`.
- **Proven:** against the old `src/vtextension/wasm/remote/CMakeLists.txt`, the rule reports `src/vtextension/wasm/remote/CMakeLists.txt:261: vtextension_remote_test links WAMR and Wasmtime; …` and exits 1. Against the new tree it passes.

**Wine/clang-cl cross tree.** It cannot reproduce the LNK2005. `out/clangcl-debug` has no Wasmtime (`CONTOUR_HAVE_WASMTIME` is unset there), so `vtextension_remote_test` is not even generated. The two changed `.cpp` files that have Windows branches were compiled with clang-cl: `ExecutorThread.cpp` (via ninja) and `HelperComposition.cpp` (directly from its compile command). Both are clean. That tree cannot currently link `contour-extension-host*.exe` for an unrelated reason: WAMR's `win_file.c` does `#pragma comment(lib, "Pathcch.lib")`, and msvc-wine ships `pathcch.lib`, which a case-sensitive file system does not match. This predates this work and only affects the local cross tree.

## 2. ubuntu-24.04: `vtextension_wamr_helper_test` — the helper "ended uncleanly", `loaded.has_value()` false

**Not extended-const, not WAMR, not the debug build.** Facts first:
- The CI job is not a Debug build. It passes `-DCMAKE_BUILD_TYPE=Release` before `--preset gcc-debug`, and the command line wins; WAMR's own banner in the log says Release.
- C is compiled by the default `cc` (gcc 13), C++ by clang++-22 against libstdc++ 14.
- I rebuilt that configuration twice:
  - `out/ci-ubuntu` on this host (gcc C + clang-22 C++, Release);
  - a CI-equivalent tree in an Ubuntu 24.04 container (gcc 13.3, glibc 2.39, clang-22, libstdc++ 14), sandbox confirmed active by `strace` (landlock + seccomp).
- Neither failed in a single run.
- A gcc-13 WAMR loading and instantiating the extended-const module under valgrind is clean.

**Reproduced as a timing flake.** I ran `vtextension_wamr_helper_test "WasmContract.*"` 4×25 in parallel, niced.
- 4 of 100 runs failed, in different cases each time: `refusesDisabledFeatures` (function-references, simd), `tableCountIsCapped`.
- Each failure was "1 extension host(s) ended uncleanly" and nothing else.
- So the CI's extended-const was simply the section that lost the race.

**Root cause, with evidence.**
1. Once the error log was routed to the test's stderr (see below), every failure read `[error] An extension host ended unexpectedly (signal 31).` Signal 31 is SIGSYS: the seccomp filter (`SECCOMP_RET_KILL_PROCESS`).
2. A temporary experiment switched the filter's kill to `SECCOMP_RET_TRAP` with a SIGSYS handler that prints the system call. Helpers still died by plain SIGSYS and the handler never printed. A trapped system call kills anyway when SIGSYS is blocked, so the offending thread had every signal blocked. That is a thread during the C library's start-up or exit. (This experiment was reverted.)
3. `HostProcess::run()` starts the `InterruptListener` thread and then the server. The server waits, on a latch, only for its lane executors to prepare, and then locks down. Nothing waits for the listener thread to actually start.
   - A new thread first runs glibc's start-up with all signals blocked: `rseq()`, `set_robust_list()`, then a mask restore. Neither `rseq` nor `set_robust_list` is in any row.
   - On a loaded machine, the listener's start-up can run after `seccomp(SECCOMP_FILTER_FLAG_TSYNC)`, and the kernel kills the host silently.
   - This is the defect in `14b6ad43e` ("locks down in a fixed order, its threads prepared first"), whose message promises lockdown "only once every thread runs".

**Fix.** `ExecutorThread::start()` now returns only once the new thread has entered its body: a `std::latch` in `State` is counted down by `runBody` before `body()` runs. Every thread the host starts (the listener, lanes and anything later) therefore has its start-up behind it before any confinement step. That covers the general case, not just the listener.
- `State` is built in place (`std::make_unique<State>(std::move(body))`), since a latch cannot move.
- New test: `ExecutorThread.startReturnsOnceTheBodyRuns`.
- After the fix: **0 failures in 4×60 = 240 parallel runs**, against 3–4 per 100–160 before, on the same tree and load.
- Folded into `14b6ad43e` → **`9ec39e8cf`**, message extended.
- I added no row for `rseq`/`set_robust_list`. The thread must not be starting at lockdown at all, and allowing them would only move the race to the next start-up call.

**The report now reaches the test output.**
- The supervisor already logged `An extension host ended unexpectedly (exit code|signal N).` to `errorLog`. But a program's console sink stays disabled until configured, so tests printed nothing. That is why CI's "see their report above" had nothing above it.
- `HelperComposition` (test support, every helper suite) now points `core::log::errorLog`'s sink at the binary's original stderr, the same copy taken before any case captures stderr, which the helpers inherit. Lines are prefixed `[error] `.
- A seccomp-killed helper, which cannot speak for itself, now leaves `[error] An extension host ended unexpectedly (signal 31).` beside the run-gate failure. A crash shows signal 11 or 6, plus the helper's own report as before.
- Folded into `b945479c6` → **`0a0cb1909`** (the run-gate commit), message extended.

**Extra tree.** `out/ci-ubuntu` was used and then **deleted**. The container and its build directory were removed.

## 3. macOSArm

Still **pending** (queued, 0 s) for `9fb129082` at the end of this task (`gh pr checks 2431`). Nothing to read or fix yet.

## Local checks (all on the final tree)

- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: **9/9 passed**. The suites:
  - `vtextension_test`
  - `vtextension_wasmtime_test`
  - `vtextension_process_test`
  - `vtextension_remote_test`: 309 cases, 308 passed, 1 skipped, 12 445 assertions
  - **`vtextension_remote_wamr_test`** (new): 43 cases, 528 assertions
  - `vtextension_helper_test`
  - `vtextension_wamr_helper_test`
  - `check_wasm_runtime_containment`
  - `check_wasm_runtime_containment_selftest`: 38 cases
- `ctest --preset=clang-asan -L lint`: **34/34 passed**. This includes `check_spelling`, which runs the pinned typos; the `typos` binary itself is not on PATH here.
- `scripts/clang-tidy-diff.sh --base HEAD --build-dir out/clang-asan`, run on the changes before committing: "4 translation unit(s) clean". I did not run the whole-branch diff; the branch's other lines are unchanged from what CI already ran.
- `/usr/bin/clang-format --dry-run --Werror` on the changed C++: clean. `editorconfig-checker` on all changed files: clean. `py_compile` on the script: OK.
- Full `clang-asan` build: warning-free under `-Werror`.
- Flake stress, `out/ci-ubuntu`: 4/100 and 1/100 failures before the fix, 0/240 after.

## What CI must confirm

- Windows links `vtextension_remote_test` and the new `vtextension_remote_wamr_test` with MSVC, and both pass. The new binary is not named in `.github/actions/windows-tests/action.yml`; the regular ctest run picks it up.
- ubuntu-24.04 passes `vtextension_wamr_helper_test`. If a helper ever dies again, the log now names the signal.
- The new binary also builds on platforms without Wasmtime (FreeBSD, the no-Wasmtime jobs), where the WAMR remote contract did not run before.
- macOSArm still has to run.

## Concerns

- **Not every rewritten commit was built individually.** About 80 commits were replayed; only the final tree was built.
  - I resolved four conflicts. Three were in `src/vtextension/wasm/remote/CMakeLists.txt`: the new target block placed before the helper-test block in each later commit. Two were in `HelperComposition.cpp`'s includes.
  - I checked the folded commits by reading them. At `fb5b86dec` the new target's libraries and sources exist, and the test's inputs differ from HEAD only by additions to `BackendContract.hpp`. At `0a0cb1909` the includes and types are present.
- `HelperComposition`'s error sink is process-global within test binaries. It also makes the supervisor's *expected* error lines visible, for example kills of deliberately hostile helpers in `vtextension_helper_test`. They only show with `-V` or when a test fails.
- The local clang-cl cross tree cannot link `contour-extension-host*.exe` because of WAMR's `Pathcch.lib` pragma on a case-sensitive file system. This predates this work, but it blocks a wine run of the helper suites here.
