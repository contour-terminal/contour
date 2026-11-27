# Task 5.4 report: the default composition becomes out-of-process on Linux

Status: DONE_WITH_CONCERNS. Not pushed. BASE `149c7edac`.

## Commits (oldest first)
1. `377e12473` fix(vtextension): a host refused for a lasting reason is the last its connector starts (Item 0.1)
2. `8953a1535` test(vtextension): the sandbox probe locks down with either engine's system call rows (Item 0.2)
3. `a3f0673ba` refactor(vtextension): small cleanups in the extension host and its engines (Item 0.3)
4. `afbdf29ec` feat(vtextension): run extensions in a sandboxed extension host process by default on Linux
5. `5fa7d38c5` build: select where extensions run and which engine the extension host uses

### What each does
- **1:** `RefusalPersistence` + `RefusalPersistenceRows` in `HostRefusals.hpp`, one row per reason, with a
  compile-time check that every reason has its row at its index. `refusalPersistence()` reads it.
  `ProcessHostConnector::refused()` keeps every `Lasting` refusal (`LockdownFailed`, `Unsandboxed`,
  `ProtocolVersion`). `LockdownHistory` became `RefusalHistory`. The new test
  `ProcessHostConnector.lastingRefusalsStartNoOtherHost` runs one GENERATE row per refusal kind. It pins the
  table and proves that no second host is launched after a lasting refusal, and that one is launched after a
  transient one. `Hostility.modes/wrong-version` used to expect a relaunch. It now expects none: a new
  `NextHost` column in its table.
- **2:** the probe takes `--guest-code=interpreted|compiled` (a table; the default is compiled), and there are new
  ops `map-executable` and `protect-executable`. The new test `Sandbox.onlyCompiledCodeTurnsExecutable` runs on the
  real kernel:
  - interpreted rows: `protect-executable` and `map-executable` are both killed by SIGSYS;
  - compiled rows: `protect-executable` (RW to RX) works, and `map-executable` is still killed.
- **3:**
  - the Doxygen comment in `SeccompFilter_test.cpp` and the two WAMR probe comments are rewrapped, and the
    probe comment now mentions "no JIT";
  - `HelperEngine.cpp` now says "the extension host cannot run extensions: <why>";
  - `WasiOverrides.cpp` `symbols()` does `Require(findShimFunction(name) != nullptr)` at registration.
- **4:**
  - New: `process::HostComposition` (`create(HostCompositionConfig)`, `backend(BackendConfig)`, test hooks
    `supervisor()` and `teardownGrace()`), `teardownGraceFor(BuildInstrumentation)` (0 ms none, 2000 ms
    coverage/sanitizers), `sharedHostComposition(path)` (weak-cached under a mutex), and `remote/DefaultBackend.cpp`.
    These are in the target `vtextension_out_of_process`.
  - **Lifetime:** backends receive the composition's connector through a `shared_ptr` aliasing constructor. The
    composition therefore lives as long as the last backend, or the last runtime or slot of one, that can still
    connect.
  - **Missing helper or supervisor that cannot start:** the composition gets an `UnavailableHostConnector`, which
    logs the reason once and fails `HostUnavailable` without launching. A supervisor failure maps through a table:
    `WaitSetUnavailable` → `Unsupported`, `ThreadUnavailable` → `OutOfResources`.
  - **CMake:**
    - `CONTOUR_EXTENSION_HOST` (out-of-process|in-process) and `_CONTOUR_SANDBOXED_PLATFORMS` with the row
      `Linux:^(x86_64|AMD64|aarch64|arm64)$`. `VTEXTENSION_LINUX_SANDBOX` now derives from the same rows.
    - `vtextension_runtime` = `vtextension_out_of_process` or `vtextension_${CONTOUR_WASM_RUNTIME}`.
    - `wasm/wamr` is always added.
    - Only `contour` and `bench-headless` link `vtextension_runtime`; it moved off `contour_core` and
      `contour_cli`. `vtextension_test`, `vthost_test`, `contour_test` and `contour_gui_test` link
      `vtextension_wamr` by name.
    - `contour_place_extension_host_beside()` is called for `contour` (not in a macOS bundle) and
      `bench-headless`.
- **5:**
  - `CONTOUR_WASM_RUNTIME` defaults to empty and is resolved after the third parties: `wasmtime` if one was found
    and extensions run out of process, otherwise `wamr`.
  - Configuring fails with FATAL_ERROR in three cases: in-process + wasmtime; an unknown engine; wasmtime named
    but not found.
  - The `CONTOUR_EXTENSION_HOST_ENGINE` stopgap is gone. All of its users now read `CONTOUR_WASM_RUNTIME`.
  - The summary shows "Where extensions run" and "WebAssembly runtime".
  - The Wasmtime status text no longer says only its tests use it.

Every commit passed clang-format 22 `--dry-run --Werror`, editorconfig-checker and typos
(`spelling OK (32 exceptions, all justified)`).

## Configure checks
The script is `<scratchpad>/configure-checks.sh <source dir>`, kept at
`/tmp/claude-1000/-home-christianparpart-projects-contour/a3633faa-27fc-44d2-bb77-a2354e68aa02/scratchpad/configure-checks.sh`.
Each case runs a fresh `cmake --preset clang-asan -B <scratchpad>/cfg-<name>` with `PACE_SCHEDULER=127.0.0.1:6674`
and a shared CPM source cache, then checks the exit status and that every pattern matches the log. The tree
is deleted after each case. Nothing was built. All trees and the cache were removed at the end.

```
PASS default: ok; [|> Where extensions run: out-of-process] [|> WebAssembly runtime: wasmtime]
PASS in-process-wasmtime: fatal; [CONTOUR_WASM_RUNTIME is wasmtime, which runs extensions only in the] [extension host, but CONTOUR_EXTENSION_HOST is in-process. Choose]
FAIL no-cpm: expected ok ..., got fatal (exit 1)   -- "Could not find GSL. Run install-deps.sh, install system packages, or set CONTOUR_USE_CPM=ON."
PASS in-process-default: ok; [|> Where extensions run: in-process] [|> WebAssembly runtime: wamr]
PASS out-of-process-wamr: ok; [|> Where extensions run: out-of-process] [|> WebAssembly runtime: wamr]
PASS unknown-host: fatal; [CONTOUR_EXTENSION_HOST is "nowhere", which is not where extensions can run.]
PASS unknown-runtime: fatal; [CONTOUR_WASM_RUNTIME is "v8", which is not a WebAssembly engine Contour has]
```
`-DCONTOUR_USE_CPM=OFF` cannot configure on this machine. Neither GSL nor WAMR is installed, and CPM OFF makes
both fatal before the engine is chosen. The two substitute cases `in-process-default` and `out-of-process-wamr`
exercise the WAMR branch of the defaulting and WAMR in the extension host. The exact branch "out-of-process,
no Wasmtime found, default → wamr" was never reached by a configure. It is the `else()` of a two-line `if` in
the top-level CMakeLists.txt, reviewed by reading only.

## nm proofs (out/clang-asan, final head)
```
nm -C src/contour/contour | grep -cE 'wasm_runtime_|wasmtime_'                                  -> 0
nm -C src/vtbackend/bench-headless | grep -cE 'wasm_runtime_|wasmtime_'                        -> 0
nm -C src/contour-extension-host/contour-extension-host | grep -cE 'wasm_runtime_|wasmtime_'   -> 7471 (all wasmtime_)
```

## Tests (clang-asan)
- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: 8/8 passed on the final head: the 6
  vtextension suites and the 2 containment lint gates. With `|bench_headless` added, 19/19 passed. That includes
  `bench_headless_throughput_extensions_smoke` and `bench_headless_memory_extensions_smoke`, which run through
  helpers now.
- New cases:
  - `HostComposition.*`: 22 assertions in 3 cases;
  - `ProcessHostConnector.*`: 46 assertions in 2 cases;
  - `Sandbox.*`: 372 assertions in 7 cases;
  - `Hostility.modes`: 1418 assertions.
- Tests that now name WAMR's backend:
  - `vthost_test "[extensions]"`: 221 assertions in 22 cases;
  - `contour_test`: 40635 of 40635 assertions;
  - `contour_gui_test "*xtension*"` offscreen: 398 assertions in 32 cases.
- **bench-headless helper proof:**
  - Under `strace -f -e trace=execve`, `bench-headless throughput ... extensions echo.wasm,hostile.wasm` shows
    two `execve(".../src/vtbackend/contour-extension-host", ["--control=3", "--lanes=4,5,6,7", ...])`.
  - Run without strace it exits 0 with no sanitizer report.
  - Under strace, LeakSanitizer itself aborts ("does not work under ptrace"). That is expected.
- **e2e:** `QT_QPA_PLATFORM=offscreen ctest --preset=clang-asan -L e2e`: 35/35 "passed", of which 17 ran and
  passed, including daemon, hosting, live upgrade, crash recovery and cli verbs. The 18 `contour_e2e_display_*`
  cases skip without a display. Per the run rules they were not run on a display.

## Manual check (offscreen, PID-recorded)
`contour config <scratch>/contour.yml daemon-mode in_process execute sh -c 'sleep 8'` ran with
`QT_QPA_PLATFORM=offscreen`, with a scratch HOME, XDG_CONFIG_HOME and XDG_RUNTIME_DIR. The profile had `clock`
and `echo` from the build tree as `{Extension:...}` segments of the indicator status line. contour was PID
3067260.

`pgrep -P 3067260` found:
```
3067294 contour-extensi  NoNewPrivs: 1  Seccomp: 2
3067301 sleep            NoNewPrivs: 0  Seccomp: 0
3067305 contour-extensi  NoNewPrivs: 1  Seccomp: 2
```
contour exited on its own once the shell ended, and both helpers were gone afterwards. Its stderr has two lines
"contour-extension-host: resource caps are off in this build", no refusal line, no "disabled" notice, and no
ASan or LSan report from contour or the helpers; only LSan's suppression table was printed. This verifies the
controller's request: a sanitizer-built contour starts helpers that exit cleanly, with no environment row.

The rendered status-line text itself was not captured. Offscreen there is no grab of the grid in this check.
"Renders" rests on the absence of failure notices, and on `contour_gui_test`'s extension cases and the
bench-headless extension rows.

## Performance impact
- The default changes where guests run, not the terminal's hot path. `bench-headless throughput` ascii/primary with
  two extensions read 28.8 to 30.4 MiB/s (1.01 Ginstr) in the ASan build, through helpers. No before/after
  measurement was taken in a release build.
- Each extension now costs a helper process, about 49 MB of executable on disk in ASan builds, shared pages at
  run time.
- Shutting down an instrumented build may take up to 2 s for helper teardown grace. Release builds take 0 s.

## Deviations
1. **`contour_place_extension_host_beside()` is defined in `src/CMakeLists.txt`**, not in
   `src/contour-extension-host/CMakeLists.txt`.
   - `bench-headless` is defined in `vtbackend/`, which is read before that directory.
   - `add_custom_command(TARGET ... POST_BUILD)` and an `OUTPUT` in the target's binary directory must be issued
     in the target's own directory.
   - It is a command whose OUTPUT is the placed helper, as the macOS bundle copy already does, rather than a
     POST_BUILD step. It therefore reruns when only the helper changes.
   - It does nothing where `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is set (Windows, where every executable already shares
     `bin/`).
2. **`_CONTOUR_SANDBOXED_PLATFORMS` rows carry a processor pattern** (`Linux:^(x86_64|AMD64|aarch64|arm64)$`),
   not just a system name. Linux on another architecture (riscv64, ppc64le, ...) has no seccomp rows. There every
   helper would be refused `Unsandboxed` and extensions would never run, so it stays in-process. The Linux sandbox
   switch reads the same rows, so the processor list exists only once.
3. **`vthost_test`, `contour_test` and `contour_gui_test` link `vtextension_wamr` by name.** The brief named only
   `vtextension_test`. `vtextension_runtime` moved from `contour_core`/`contour_cli` to the `contour` executable.
   The OBJECT/static libraries propagated it into those tests, which would otherwise have composed out-of-process
   backends with no helper beside them, against D100's "tests do not silently start spawning helpers".
4. The no-CPM configure check could not pass here; see above.
5. `ProcessLauncher.hpp`/`Process_*.cpp` are unchanged, as the controller note said: no sanitizer environment row.
6. `scripts/check-wasm-runtime-containment.py` is unchanged: no rule tripped.

## Concerns
- **Existing build trees keep `CONTOUR_WASM_RUNTIME=wamr`.** The old default was cached explicitly, so a tree
  configured before this change keeps `wamr` until `-UCONTOUR_WASM_RUNTIME` or `-DCONTOUR_WASM_RUNTIME=`. Such a
  tree runs a WAMR helper, which works. `out/clang-asan` here was reset with `-UCONTOUR_WASM_RUNTIME`.
- **`vtextension_process_test` now links `vtextension_out_of_process`**, and with it vtextension, vtbackend and
  vtpty. It still has no engine, but the Windows cross build and the wine run of that suite were not repeated.
  Neither was any macOS or FreeBSD build. `vtextension_out_of_process` exists only where a process launcher
  exists. Choosing out-of-process without one is a FATAL_ERROR.
- **The weak-cached `sharedHostComposition()` keeps one slot** (helper path plus weak_ptr). A process asking for
  two different helper paths at once would get two compositions, the second replacing the first in the cache.
  No caller does this today.
- **Per-commit builds:** commits 1 to 3 were built and tested at their own state. The feat commit was built with
  the full target list on the stopgap engine. The build commit was built in full. After the final two
  autosquashes, only HEAD was rebuilt; the absorbed changes were a test table and a comment.
- **`currentExecutablePath()` follows `/proc/self/exe`.** After a package upgrade replaces the binary on disk,
  that path ends in " (deleted)", and a backend created afterwards would not find the helper. Today backends are
  created at startup.

## Fix round 1

The branch was rebased with `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash origin/master`, after a
`git fetch origin master`. origin/master was still `8c3afaa0c`. This task's commits are now:

- `d709578b2` fix: lasting refusals
- `f993f1ca0` test: probe guest code
- `2a9c80a1a` refactor: cleanups
- `30ad6c0a9` feat: out-of-process default
- `4647dde32` build: engine selection

Not pushed.

### Items, fixes, and the commit each was folded into
| # | Item | Fix | Folded into |
|---|------|-----|-------------|
| A1 | The .deb lacked the helper | Both `install(TARGETS contour-extension-host ...)` rules got `COMPONENT contour`, with a comment saying why. CI now checks for the helper in three places: the daemon .deb inspection (`dpkg-deb -c ... \| grep -Eq '\./usr/libexec/contour/contour-extension-host$'`, with an `::error::`), the GUI .deb CPack step (the same grep under `set -ex`), and the GUI "Verify installation" step (`test -x /usr/libexec/contour/contour-extension-host`). Other generators: WIX/ZIP pack `Unspecified contour contour_path`, so they were not affected, and they still get it through `contour`. DragNDrop carries the helper in the bundle (unchanged). The RPM spec uses the plain install. Verified with `cmake --install out/clang-asan --component contour --prefix <scratch>`, which yields `libexec/contour/contour-extension-host`. | install rule: `b49445a1d` (build: build the extension host with whichever engine the build selected), the last commit to touch those lines; CI checks: `39a112390` (build: install contour-extension-host beside contour) |
| A2 | `HostComposition::create` took its config by value | It now takes `HostCompositionConfig const&` | `30ad6c0a9` |
| A3 | AppImage | `build-appimage.sh` passes `--executable "$APPDIR/usr/libexec/contour/contour-extension-host"` to linuxdeploy whenever the helper is installed. **Verified:** `bash -n`, and `readelf -d` of the built helper shows NEEDED libstdc++, libgcc_s, libpng16, libz, libssh2, libutempter, libsystemd and no RUNPATH. So it does need linuxdeploy's RPATH to reach the bundled libstdc++. **Not verified:** the AppImage itself. linuxdeploy is not installed here, and no release AppImage was built, so the helper's RUNPATH inside an AppImage is unproven. | `39a112390` |
| A4a | `DefaultBackend.cpp` with a caller's connector | `makeDefaultBackend` returns `makeRemoteBackend(makeRemoteKindState(), config)` at once: it neither looks for a helper nor builds a composition | `30ad6c0a9` |
| A4b | Shared connector across lockdown requirements | `sharedHostComposition(helper, LockdownRequirement, writer)` keeps one weak slot per requirement, and the comment says one path is cached per requirement. The new test `HostComposition.eachLockdownRequirementHasItsOwn` checks it. | `30ad6c0a9` |
| A4c | Doxygen wrap in `HostComposition.cpp` | Rewrapped | `30ad6c0a9` |
| A4d | `HostileRow` doc comment | Moved back above `struct HostileRow`; `NextHost` has its own | `d709578b2` |
| A4e | Positive control | New probe op `protect-readable` (mprotect to PROT_READ). The test expects it to work (Clean, `op=ok`) under the interpreted rows. | `f993f1ca0` |
| A4f | Test for the helper-not-found path | The not-found logic moved into `process::makeProcessBackend(config, helper, writer)`, which `makeDefaultBackend` calls with `installedHelper()` and `errorLogWriter()`. The new test `HostComposition.defaultBackendWithoutHelperRefusesEveryLoad` checks one `HelperNotFound` line through the injected writer, and three loads that all fail with `HostUnavailable`. | `30ad6c0a9` |
| A4g | Comment on the cache | In `sharedHostComposition()`'s doc comment and body | `30ad6c0a9` |
| B5a | `misc-unused-using-decls` (`HostProcess_test.cpp`) | Removed the using | `f710c2cd6` (the commit that removed its last use) |
| B5b | `pro-type-member-init` (`SeccompFilter_test.cpp:375`) | `call {}`, `arguments {}`, `what = nullptr` | `dbbdf77d9` |
| B5c | `NewDeleteLeaks` (`WamrEngine_test.cpp:139`) | The analyzer followed the opener into a heap-stored `std::function` that captured another `std::function` (`systemNullDeviceOpener()`), and lost it. That opener could never be called in that case, so the case now passes a small counting lambda that opens nothing; the refusal comes before any open. Reproduced with `clang-tidy` 22.1.8 before the fix and clean after it. | `dbbdf77d9` |
| B5d | `suspicious-stringview-data-usage` (`WasiOverrides.cpp`) | `WasiOverride::name` became `char const*` (NUL-terminated). The module name is a `constexpr char const* WasiModuleName`, with `static_assert(std::string_view { WasiModuleName } == WasiModule)`. | `e9965ccd4`; the cleanup commit's lines were rebased onto it by hand |
| B6 | MSVC: `__builtin_frame_address` | Under `_MSC_VER && !__clang__` the test uses `_AddressOfReturnAddress()` (`<intrin.h>`). Grepping the branch diff for `__builtin_` found two other uses: `__builtin_available` in `PosixLaunch.cpp` (POSIX/Apple only) and `__builtin_trap` in `Codec_fuzz.cpp` (Clang only, `VTEXTENSION_FUZZING` refuses other compilers). MSVC compiles neither. | `3d1ba71c6`; later commits' conflicts on this file were resolved by keeping their version and reapplying the change |
| B7 | `check_no_threaded_fork` | The probe's `fork` op exists to prove the filter kills a fork, and the probe is threaded by design, so the lint's documented mechanism (its `ALLOWED` table, with a reason) is used. The reason: a test-only probe that is never shipped, whose `fork` op proves the extension host's filter kills a fork before any child exists, and whose child, unsandboxed, only `_exit()`s. A raw `syscall(SYS_clone)` would only dodge the lint. | `25c0a5fdc` |
| B8 | All lints | See below | n/a |

### Command output (HEAD `4647dde32` unless noted)
- `CLANG_TIDY=/usr/bin/clang-tidy JOBS=4 ./scripts/clang-tidy-diff.sh --base origin/master --build-dir out/clang-asan`
  (clang-tidy 22.1.8): `clang-tidy-diff: 146 translation unit(s) clean.` The two `*_win32.cpp` files are not in
  this tree's compile_commands.json. This ran on the pre-rebase head, whose tree differs from HEAD only in where
  the `<intrin.h>` block sits in `HostServer_test.cpp`.
- `ctest --preset=clang-asan -L lint`: `100% tests passed, 0 tests failed out of 33`.
- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: `100% tests passed, 0 tests failed out of 8`.
- `check-no-threaded-fork.py`: `1836 files are free of fork().`, and `--self-test`: `18 cases OK`.
- Focused runs:
  - `HostComposition.*`: 33 assertions in 5 cases;
  - `Sandbox.onlyCompiledCodeTurnsExecutable`: 69 assertions;
  - `ProcessHostConnector.*`: 46 assertions;
  - `Hostility.modes`: 1418 assertions.
- `/usr/bin/clang-format --dry-run --Werror` on every C++ file the branch changes: clean. `editorconfig-checker`:
  clean. typos: `spelling OK (32 exceptions, all justified).`
- nm: `contour` 0 and `bench-headless` 0.
- **Per-commit builds:**
  - `30ad6c0a9` (commit 4) checked out alone, configured with `-DCONTOUR_WASM_RUNTIME=wamr`, since its CMake still
    rejects an empty value. It built `vtextension_process_test`, `contour`, `bench-headless`, `vtextension_test`,
    `vtextension_helper_test`, `vtextension_remote_test`, `vthost_test` and `contour_test` with exit 0 and no
    warnings. Its `HostComposition.*` cases ran: 33 assertions in 5 cases passed.
  - HEAD: full build, exit 0.
  - The earlier commits folded into (`39a112390`, `b49445a1d`, `25c0a5fdc`, `f710c2cd6`, `3d1ba71c6`, `dbbdf77d9`,
    `e9965ccd4`, `d709578b2`, `f993f1ca0`) were not rebuilt one by one. Their changes are test-only, CMake or
    CI/script edits, and their final content is covered by the HEAD build. For `3d1ba71c6` I checked the squashed
    diff for conflict markers and for the guarded call.

### Concerns (round 1)
- **The AppImage RUNPATH of the helper is unverified** (see A3).
- **Moved `<intrin.h>` block:** in `HostServer_test.cpp` it now sits after the `#ifndef _WIN32` include block
  rather than before it. The automated conflict resolution put it there; this has no effect on behaviour.
- **MSVC was not built locally.** The fix follows the documented intrinsic, and the clang-cl cross tree cannot
  show the C3861.

## Fix round 2

The branch was rebased with autosquash onto origin/master (`8c3afaa0c`, after `git fetch`). The rebase had no
conflicts, and the tree is identical to the pre-rebase head. This task's commits are now:

- `028001ad0` fix
- `7b5710059` test
- `7fc00235b` refactor
- `8bdf81585` feat
- `98ebe9f91` build

Not pushed.

| # | Item | Fix | Folded into |
|---|------|-----|-------------|
| 1 | The AppImage patched a copy of the helper | `build-appimage.sh` passes `--deploy-deps-only "$APPDIR/usr/libexec/contour/contour-extension-host"` (the array is now `EXTENSION_HOST_DEPLOY`), with a comment saying why `--executable` is wrong. | `a315c8b93` (build: install contour-extension-host beside contour) |
| 2 | CI package checks in the wrong commit | The three checks (daemon .deb inspection, GUI .deb CPack step, GUI install verification) were removed from the install commit and added to the commit that sets `COMPONENT contour`. `git show` now finds no `build.yml` change in `a315c8b93`, and all three checks in `76e205daa`. | removed from `a315c8b93`, added to `76e205daa` (build: build the extension host with whichever engine the build selected) |
| 3 | Hard-coded slot count | `BackendConfig.hpp` gains `isLockdownRequirement()` (an exhaustive switch with no default) and `LastLockdownRequirement`, with the two `static_assert`s `HostRefusals.hpp` already uses for `LastHostRefusal`. The slot array is sized `to_underlying(LastLockdownRequirement) + 1`. A third requirement fails to compile: -Wswitch fires under -Werror, and then the static_assert fires until `Last` names it. | `8bdf81585` |

### linuxdeploy verification (item 1)
I downloaded the pinned linuxdeploy `1-alpha-20251107-1` (build 331, git cc7b864, sha256
`c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d`) into the scratchpad and ran it with
`--appimage-extract-and-run`. Each run used a throwaway AppDir holding only the release-built helper at
`usr/libexec/contour/contour-extension-host`.

- **`--executable` (the round-1 script):** it copies the helper to `usr/bin/contour-extension-host`, with RUNPATH
  `$ORIGIN/../lib`. The original in `usr/libexec/contour/` has no RUNPATH. This confirms the finding.
- **`--deploy-deps-only` (the fix):**
  - No copy appears in `usr/bin`.
  - The helper in `usr/libexec/contour/` gets RUNPATH `$ORIGIN/../../lib:$ORIGIN`.
  - `ldd` resolves `libutempter.so.0` to the AppDir's `usr/lib`.
  - `libstdc++` resolved to the host's in this throwaway AppDir, because linuxdeploy excludes it by default.
    The real script adds it to `usr/lib` with `--library`, where this RUNPATH reaches it.

A full AppImage was not built: no release Qt and plugin setup was run. The scratch download was deleted.

### Checks (HEAD `98ebe9f91`)
- `bash -n scripts/build-appimage.sh`: ok.
- Full ASan build: exit 0, 0 warnings.
- `HostComposition.*`: 33 assertions in 5 cases passed.
- `ctest -L lint`: 33 of 33 passed. `ctest -R 'vtextension|wasm_runtime_containment'`: 8 of 8 passed.
- clang-format, editorconfig-checker and typos are clean on the changed files. clang-tidy 22.1.8 on
  `HostComposition.cpp`: 0 findings.
- The other changed commits were not rebuilt on their own: commit 4 (`8bdf81585`, BackendConfig and
  HostComposition), and `a315c8b93` and `76e205daa` (script and CI YAML only).
