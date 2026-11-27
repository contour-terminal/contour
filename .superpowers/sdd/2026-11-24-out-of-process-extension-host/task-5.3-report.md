# Task 5.3 report: WamrEngine, and an extension host that runs it

Status: DONE_WITH_CONCERNS (see Concerns).

## Commits

Base `67e99c3d9`. Item 0 first, then the two feature commits. Every commit was built on its own (see Tests).

| commit | subject |
|---|---|
| `750f0080c` | fix(vtextension): a crash in the locked-down extension host is no longer taken for a sandbox kill (item 0.1, 0.3) |
| `ccfd77033` | fix(vtextension): an outer filter refusing one argument value lets every other value through (item 0.2) |
| `92d9750e2` | fix(vtextension): a sanitizer build's extension host reads only its own process entries (item 0.4, 0.5) |
| `e8a021e9a` | feat(vtextension): the extension host can run its modules interpreted |
| `5fdb8bfdf` | build: build the extension host with whichever engine the build selected |

`e8a021e9a` was built and tested alone (B part stashed): vtextension_test, remote_test, process_test,
helper_test, wasmtime_test, the helper and its hostile twin -- clean, and remote/process/helper suites passed.

## Item 0

1. **writev / rt_sigaction allowed** (`SystemCallRows.cpp`, release rows). `SystemCall::Writev` added (x86_64 20,
   aarch64 66) to the enum and `NumberTable`; `RtSigaction` already had both numbers. Each row says why.
   `SeccompFilter.decidesEachRowAsItsTableSays` now pins `rt_sigaction` **allowed** (it pinned it killed) and
   `writev` allowed, on both architectures.
2. **`unlisted` overload**: `RowBody::finish(decision, failed)` -- a failed argument check now jumps to a return of
   the filter's unlisted action, not to `SECCOMP_RET_KILL_PROCESS`. The host's own filter (unlisted = Kill) is
   unchanged; an outer allow-by-default filter lets every other value through. Documented at the file comment and at
   the overload (`SeccompFilter.hpp`). New evaluator test `SeccompFilter.failedChecksTakeTheUnlistedAction` (both
   unlisted actions; matching value refused, other value and other call take the unlisted action). The probe's outer
   filters needed no row change: `passrights` still refuses `setsockopt(SO_PASSRIGHTS)` with EPERM; other
   `setsockopt` calls, which it used to kill, now pass -- which is the fix.
3. **`SystemCallRows.cpp` stat comment** now says seccomp cannot read the path, that a path stat remains possible,
   and that this is accepted residual risk (metadata observable; Landlock does not govern a stat).
4. **`/proc/self`** instead of `/proc` in the sanitizer build's Landlock rule (`O_PATH` open resolves it to
   `/proc/<pid>`). Added two probe ops and expectations: `open-own-process` (`/proc/self/maps`: allowed in the
   sanitizer build, EACCES otherwise) and `open-other-process` (`/proc/<parent>/maps`: EACCES in every build). The
   sanitizer `vtextension_helper_test` passes sandboxed (160 cases).
5. **`SanitizerOptions.cpp`** is compiled only where `VTEXTENSION_LINUX_SANDBOX` (Linux x86_64/aarch64); elsewhere
   `vtextension_host_sanitizer_options` is an empty INTERFACE target so the link lines stay unconditional. The comment
   says why (ELF labels; Mach-O's would carry an underscore).

## Phase-5 facts

- **F (D98 spike): WAMR 2.4.5 has no linear-memory placement hook a system WAMR exposes. Memory mode is Copy, final
  for this branch.** Read in `out/clang-asan/_deps/wamr-src`:
  - `core/iwasm/common/wasm_memory.c`: `wasm_allocate_linear_memory()` maps through `wasm_mmap_linear_memory()` ->
    `wasm_mremap_linear_memory(NULL, ...)` -> `os_mmap(NULL, ...)` (no base parameter), and grows in
    `wasm_enlarge_memory_internal()` by `os_mremap`/`os_mprotect` of WAMR's own mapping (or, under
    `OS_ENABLE_HW_BOUND_CHECK`, an 8 GiB reservation of its own).
  - The only seam is compile-time: `WASM_MEM_ALLOC_WITH_USAGE` (`WAMR_BUILD_ALLOC_WITH_USAGE`,
    `build-scripts/config_common.cmake`), which routes linear memory through the `Alloc_With_Allocator`
    `malloc_func`/`realloc_func` of `RuntimeInitArgs.mem_alloc_option` with `Alloc_For_LinearMemory`
    (`wasm_export.h`: `mem_alloc_usage_t`). It is off by default, changes the allocator callbacks' signatures (an ABI
    switch with no runtime query), and is process-global (one `allocator_user_data`), not per instance. A distribution
    WAMR built with defaults (Fedora, FreeBSD ports) does not have it, and the configure probe could only detect it by
    a try_run that calls through a signature it cannot know. `wasm_runtime_create_shared_heap()` is an extra region
    beside the linear memory, not the memory itself.
  - Rule applied: no hook a system WAMR exposes -> Copy. No `cmake/ContourThirdParties.cmake` change.
- **F: the WAMR build is interpreter-only** (`WAMR_BUILD_INTERP 1`, `FAST_INTERP 1`, `AOT 0`, `JIT 0`,
  `FAST_JIT 0`; a system WAMR is accepted only after `cmake/WamrTerminateProbe.cpp`). Measured: no `mmap` or
  `mprotect` with `PROT_EXEC` after lockdown in either tree. So the WAMR helper's rows drop `PROT_EXEC` from
  `mprotect` entirely (see below).
- **F: row sets are chosen by the engine.** `IHostedEngine::guestCode()` (`GuestCode::{Interpreted, Compiled}`,
  `host/GuestCode.hpp`) reaches `makeSystemSandbox(descriptors, code)`; `rowsFor(code, instrumentation, arch)` =
  release rows, then the engine's set, then the instrumentation's, each later set replacing a row of the same call.
  Compiled = `mprotect` never W+X, aarch64 `membarrier` (moved out of the release rows unchanged). Interpreted =
  `mprotect` never executable, `fcntl(F_GETFL)` only, `lseek`, `sched_getaffinity` refused ENOSYS.
- **F: WAMR's WASI touches its standard-stream descriptor at every instantiation**: `fstat`, `ioctl(TCGETS2)`
  (refused ENOTTY, as for Wasmtime), `fcntl(F_GETFL)`; a guest's `fd_write` is `writev`, `fd_seek` is `lseek`, on the
  shared null device. WAMR's exec-env creation calls `pthread_getattr_np()`, which on an executor thread asks
  `sched_getaffinity` (refused ENOSYS: glibc then answers without the CPU set).
- **F: the WAMR helper holds WAMR's runtime for the engine's life.** `prepareThread()` runs before any runtime
  exists, and `ThreadEnv::enter()` needs one up; the engine takes one `acquireRuntime()` hold at construction (before
  lockdown), so a thread's environment is entered then and stays valid (one generation) for the helper's life.
- **F: `CONTOUR_WASM_RUNTIME` accepts only `wamr` on this branch** (`_CONTOUR_WASM_RUNTIMES`), so "the helper links
  the engine `CONTOUR_WASM_RUNTIME` names" could not be followed literally without moving the existing Wasmtime
  helper and its suite to WAMR. See Deviations, D1.

## Strace (D93)

Method: as in 5.2, with a temporary, uncommitted measuring change -- `Sandbox_linux.cpp`'s `lockDown()` assembled the
real rows with `SystemCallAction::Allow` as the unlisted action, so that (with item 0.2) nothing is killed and the
refused rows still refuse. The marker is each helper's `seccomp(SECCOMP_SET_MODE_FILTER)`. `strace -f -qq -ttt` over
the whole `vtextension_wamr_helper_test`; the scratchpad script `after_lockdown.py` groups threads by helper
(clone3 results; an undecoded clone3 inside the helper is a thread) and counts each helper's calls after its marker.
107 helpers locked down in each tree.

| syscall | release | asan | row |
|---|---|---|---|
| shutdown | 3150 | 3150 | allow |
| recvfrom | 2836 | 2831 | allow |
| sendto | 2268 | 2268 | allow |
| mmap | 203 | 3110 | allow; none with PROT_EXEC |
| madvise | 527 | 3088 | allow |
| munmap | 390 | 1918 | allow |
| rt_sigprocmask | 527 | 1577 | allow |
| close | 1050 | 1050 | allow |
| recvmsg | 810 | 816 | allow on control |
| fcntl F_GETFL | 595 | 595 | **new: interpreted rows, F_GETFL only** |
| fstat | 594 | 594 | allow |
| ioctl TCGETS2 | 594 | 594 | refused ENOTTY |
| sigaltstack | -- | 525 | allow |
| exit | 525 | 525 | allow |
| futex | 116 | 231 | allow |
| mprotect | 153 | 148 | **interpreted rows: never PROT_EXEC** (none measured) |
| poll | 110 | 110 | allow |
| write | 105 | 105 | allow |
| exit_group | 105 | 105 | allow |
| brk | 95 | 0 | allow |
| sched_getaffinity | 63 | 63 | **new: interpreted rows, refused ENOSYS** |
| mremap | 4 | 4 | allow |
| writev | 2 | 2 | allow (item 0.1) |
| lseek | 1 | 1 | **new: interpreted rows** |
| restart_syscall, getrandom, sched_yield | 1 each | 1 each | allow |
| openat | 1 (EACCES) | 0 | refused (glibc's overcommit probe), sanitizer allows |

Not measured, kept from the release rows: the abort path, `clock_gettime`, `kill`/`tgkill`, etc. The final rows were
then run for real: the WAMR helper suite passes locked down in both trees (below).

## Tests and commands

All in `out/clang-asan` unless noted; `unset $(env | grep -o '^CONTOUR_[A-Z_]*')` in every shell.

- Item 0 (after the three item-0 commits' changes): `vtextension_process_test` -> 127 cases, 126 passed, 1 skipped
  (4535 assertions); `vtextension_helper_test` whole -> 160 cases, 159 passed, 1 skipped (3883 assertions, sanitizer
  helper sandboxed with the `/proc/self` rule); `vtextension_process_test "[sandbox]"` with the two new `/proc`
  probe ops -> 19 cases, 3708 assertions passed.
- `vtextension_test "WamrEngine.*"` -> 3 cases, 22 assertions passed.
- `vtextension_test "[wasm-contract]"` -> 42 cases, 494 assertions passed.
- `vtextension_remote_test "[wasm-contract]"` (Remote, RemoteCopy, RemoteWamr) -> 126 cases, 1482 assertions;
  `"*RemoteWamrTraits"` alone -> 42 cases, 494 assertions.
- `vtextension_process_test "SeccompFilter.*"` -> 7 cases, 4017 assertions (incl. the new
  `failedChecksTakeTheUnlistedAction` and `anInterpreterMakesNothingExecutable`).
- `vtextension_wamr_helper_test` whole -> 61 cases, 1203 assertions passed (25 consecutive runs green; one earlier
  run had 3 failures, see Concerns). `"Helper.wamrHelperSaysCopy"` -> 1 case, 11 assertions.
- Whole suites: `vtextension_test` 481 cases / 26441 assertions; `vtextension_remote_test` 12755 assertions;
  `vtextension_helper_test` 3883; `vtextension_wasmtime_test` 1157; `vtextension_process_test` 128 cases / 5339 --
  all passed.
- **Release tree** (`out/clang-release`, reconfigured with PACE): `vtextension_wamr_helper_test` 3 runs -> 61 cases,
  1203 assertions passed each; `vtextension_helper_test` -> 160 cases, 3896 assertions passed;
  `vtextension_process_test` -> 128 cases, 5341 assertions passed.
- `ctest --preset=clang-asan -R "vtextension|containment"` (final tree) -> 8/8 passed (containment, selftest,
  vtextension, wasmtime, process, remote, helper, wamr_helper).
- Full `cmake --build --preset clang-asan -j 4` (GUI and all tests) -> exit 0, no warnings.
- `/usr/bin/clang-format --dry-run` on every changed C++ file -> clean; `editorconfig-checker` on changed files ->
  clean.
- `/simplify` by hand: reused `ProcessDefaultBackend.cpp` instead of a copy (D2), one `overlay()` for both row-set
  layers, reverted a clang-format reorder of the untouched `DefaultBackend.cpp`, updated `HelperEngine.hpp`'s doc
  (memory mode is the engine's now).

Performance: none on the GUI path (in-process WAMR unchanged apart from the null device now held through a
`shared_ptr`). Risk: moderate -- the Linux filter's row composition changed (mprotect/membarrier moved to the
compiled set; the Wasmtime helper suites pass in both trees). Coverage: not measured in the coverage tree (only
ASan and release were run, per the brief's limits).

## Deviations, with reasons

- **D1 -- which engine `contour-extension-host` links.** The brief: "links
  `vtextension_${CONTOUR_WASM_RUNTIME}_helper_engine`; with Wasmtime present and the runtime `wasmtime`,
  `contour-extension-host-wamr` is also built". On this branch `CONTOUR_WASM_RUNTIME` can only be `wamr` (task 5.4
  adds `wasmtime` and its defaulting). Followed literally, every build would ship a WAMR helper and the Wasmtime helper
  suite (`vtextension_helper_test`, which expects Shared memory) would break. I added
  `CONTOUR_EXTENSION_HOST_ENGINE` in `src/CMakeLists.txt`: `wasmtime` where `CONTOUR_HAVE_WASMTIME`, else
  `${CONTOUR_WASM_RUNTIME}` -- exactly the default task 5.4 gives `CONTOUR_WASM_RUNTIME` out of process. The helper,
  its hostile twin and the suites follow that one variable; 5.4 can replace its two-branch `if` with
  `set(CONTOUR_EXTENSION_HOST_ENGINE ${CONTOUR_WASM_RUNTIME})`. `contour-extension-host-wamr` is built (test-only,
  never installed) where that engine is not WAMR; `vtextension_wamr_helper_test` runs against it there, and against
  `contour-extension-host` itself where the helper is WAMR's. `vtextension_helper_test` is gated on the helper being
  Wasmtime's (it was gated on `TARGET vtextension_wasmtime`, the same today). The status line goes.
- **D2 -- no `remote/testing/WamrHelperDefaultBackend.cpp`.** It would have been a copy of
  `testing/ProcessDefaultBackend.cpp` (and its clean-exit listener) differing only in `VTEXTENSION_HELPER_PATH`, which
  is a compile definition already. `vtextension_wamr_helper_test` compiles `ProcessDefaultBackend.cpp` with the WAMR
  helper's path.
- **D3 -- the rows are chosen by the engine through a new `IHostedEngine::guestCode()`** and a second parameter to
  `makeSystemSandbox()` / `HostConfinement::sandbox`. The brief allowed "a row set chosen by the engine or build"; a
  build-time choice would not work where one build runs both helpers (this one). `SandboxDescriptors` stays
  descriptors only.
- **D4 -- files added beyond the brief's list:** `host/GuestCode.hpp`; `wamr/RemoteWamrEngine_test.cpp` (the
  `[wasm-contract]` composition in remote_test: it must live in `wamr/` because the containment gate forbids WAMR
  headers elsewhere); `remote/WamrHelper_test.cpp` (`Helper.wamrHelperSaysCopy`; `Helper_test.cpp` is the Wasmtime
  helper's and asserts Shared).
- **D5 -- targets:** `vtextension_wamr_engine` (WamrEngine.cpp; links `vtextension_remote_wire`, which
  `vtextension_wamr` -- linked by the GUI -- does not need) and `vtextension_wamr_helper_engine`.
- **D6 -- `ReviewFocusProcess_test.cpp`'s real helper is held to `HelperLockdown`** (RequireLockedDown on Linux)
  instead of AcceptUnsandboxed, as the brief asks for the WAMR suite; this also applies to the Wasmtime suite, whose
  helper locks down on Linux too. Its hostile case still starts `contour-extension-host-hostile`, which runs the build's
  helper engine (Wasmtime here); there is no WAMR hostile twin.
- **D7 -- the probe's System sandbox uses `GuestCode::Compiled`** (the wider set): what it forbids, both sets forbid.
  The interpreted set is proven by evaluator tests and by the WAMR helper suite running locked down.
- **D8 -- Step 4 ("build; expect failure")** was not run as a separate red step: tests and implementation were written
  together. The first real run of the WAMR helper suite did fail (every helper killed by SIGSYS at `fcntl(F_GETFL)`),
  which is what drove the measurement.
- **D9 -- `ctest -R vtextension` ran at the end** (before the feature commits), not before each item-0 commit; the
  item-0 changes were checked with the process and helper suites before they were committed.

## Concerns

- **One flaky run.** The first WAMR helper suite run after a fresh build (ASan) had 3 cases fail the clean-exit gate
  ("1 extension host ended uncleanly": `acceptsEnabledFeatures`, `callsExport`, `Bundled: agent-status`). 23
  consecutive runs since then, plus ctest and 3 release runs, passed. I could not reproduce it to see the helper's
  report. It may be a cold-start/teardown-grace timing under load (another session is building on this machine), or a
  WAMR-helper-specific exit issue. Worth watching in CI.
- **The main test thread keeps a WAMR thread environment** (`thread_local std::optional<ThreadEnv>` in
  `WamrEngine.cpp`) after `WamrEngine.prepareThreadIsIdempotent`; harmless to the rest of `vtextension_test` (passes
  whole), but process-wide state left behind by a test.
- **`sched_getaffinity` refused ENOSYS** relies on glibc's `pthread_getattr_np` treating ENOSYS as "no affinity"
  (it does; musl does not call it). Allowing it instead would let the host read any process's CPU mask.
- **`CONTOUR_EXTENSION_HOST_ENGINE` anticipates 5.4** (D1); 5.4 must fold it into `CONTOUR_WASM_RUNTIME`.
- **`make_shared<OpenNullDevice>` after the open** could leak the descriptor on `bad_alloc`; not handled.

## Fix round 1

History after the round (rebased with `--autosquash` onto `67e99c3d9`; the coordinator force-pushes):

| commit | subject |
|---|---|
| `750f0080c`, `ccfd77033`, `92d9750e2` | item 0, unchanged |
| `cfb1fb298` | feat(vtextension): the extension host can run its modules interpreted (was `e8a021e9a`; the runtime JIT refusal folded in, message gained one paragraph) |
| `22e9e4e85` | build: build the extension host with whichever engine the build selected (was `5fdb8bfdf`, unchanged) |
| `0635a4f2b` | fix(vtextension): every engine withholds the CPU-time clocks alike (finding 1) |
| `ffc928a09` | build: refuse an installed WAMR that compiles to native code (finding 2, configure half) |
| `05046ba58` | build: package the extension host in the Fedora RPM (finding 3) |

`cfb1fb298` was checked out and built alone (vtextension_test, remote_test, helper, helper_test; `WamrEngine.*` 4/4).

### 1. clock_res_get / CPU-time clocks

- **Answer chosen for both engines: NOSYS for ids 2 and 3** (Success for 0 and 1, Inval for 4), i.e. the shim's
  existing policy. Reason: CPU time of the host process or thread is shared by every guest that host runs, so it is a
  side channel on the others; the shim withholds it deliberately (`Preview1Shim.cpp` ClockRow table), and widening
  the shim would also widen what Wasmtime guests see. A seccomp row alone would only have stopped the kill; the two
  engines (and in-process WAMR, which read real CPU time) would still disagree.
- **How:** `wamr/WasiOverrides.{hpp,cpp}` (new, in `vtextension_wamr`): a data table of WASI functions the shim
  answers in WAMR's place -- `clock_time_get` `(iIi)i`, `clock_res_get` `(ii)i`. Each is registered raw under
  `wasi_snapshot_preview1`, newer than WAMR's own libc-wasi natives (WAMR's `wasm_native_resolve_symbol` walks the
  list newest first), once per runtime bring-up (WAMR frees every registration in `wasm_runtime_destroy`), from
  `WamrRuntime::create` before the host functions. The trampoline zero-extends i32 slots, runs
  `findShimFunction(name)->call(systemEnvironment(), guestMemory, args)`, and writes the errno into slot 0. This
  applies to in-process WAMR too. No seccomp row was added for `clock_getres`: nothing reaches it now.
- **Contract:** `WasmContract.clocksAnswerAlike` (`BackendContract.hpp`, hand-assembled `ClockModule`, WAT in the
  comment): both functions for ids 0..4 on every backend. Proven to bite: with the registration disabled, in-process
  WAMR answered `0 == 52` (CPU time) and the WAMR helper case failed "1 extension host ended uncleanly" (SIGSYS) --
  then restored.
- **Every allowed WASI function through WAMR 2.4.5's posix layer** (`libc_wasi_wrapper.c` -> `posix.c` ->
  `shared/platform/common/posix/posix_file.c`), what it reaches after lockdown, and the row:

  | function | reaches | row |
  |---|---|---|
  | args_get, args_sizes_get, environ_get, environ_sizes_get | guest memory only | -- |
  | clock_time_get | now the shim: `clock_gettime(REALTIME/MONOTONIC)`, vDSO (syscall fallback) | release `clock_gettime` |
  | clock_res_get | now the shim: no call | -- |
  | fd_write | `blocking_op_writev` -> `os_writev` -> `writev` (BH_VPRINTF not defined) | release `writev` |
  | fd_close | `fd_table_detach` + `os_close(handle, is_stdio)`: no `close` for the standard streams; other fds `EBADF` | -- |
  | fd_fdstat_get | `os_file_get_fdflags` -> `fcntl(F_GETFL)` (type cached at insert) | interpreted `fcntl` F_GETFL |
  | fd_prestat_get, fd_prestat_dir_name | prestat table only | -- |
  | fd_seek | `os_lseek` -> `lseek` | interpreted `lseek` |
  | random_get | `random_buf` -> `getrandom` (CONFIG_HAS_GETRANDOM on glibc >= 2.25, `ssp_config.h`) | release `getrandom` |
  | proc_exit | `wasm_runtime_set_exception` | -- |
  | sched_yield | `sched_yield` | release `sched_yield` |
  | (instantiation) | `fstat`, `ioctl(TCGETS2)`, `fcntl(F_GETFL)` on the null device | release / ENOTTY / interpreted |

  No other gap. Noted, not changed: a guest's `fd_close(0..2)` on WAMR succeeds (the entry leaves its table; the
  shared descriptor stays open as `is_stdio`), where the shim's answer may differ; no system call is involved.

### 2. GuestCode::Interpreted is checked

- **Configure:** `cmake/WamrTerminateProbe.cpp` exits 12 if `wasm_runtime_is_running_mode_supported` is true for
  `Mode_Fast_JIT`, `Mode_LLVM_JIT` or `Mode_Multi_Tier_JIT` (after init); reason row 12 in `_WAMR_PROBE_REASONS`,
  and the FATAL_ERROR text and function doc say "without a JIT". Checked: the probe compiles against the CPM WAMR
  and, linked with the tree's `libiwasm.a`, exits 0.
- **Runtime:** `makeWamrEngine(WamrEngineConfig)` now returns `std::expected<std::unique_ptr<IHostedEngine>,
  WamrEngineError>`; it asks `config.nativeCompilation()` (default `linkedNativeCompilation()`, the three running
  modes) before opening anything and refuses with `CompilesNativeCode`. `wamrEngineErrorText()` (data table) gives
  the reason without naming the runtime; `HelperEngine.cpp` writes `contour-extension-host: no engine: <reason>` and
  the host then ends `EngineUnavailable`. `vtextension_wamr_engine` links `contour::wamr` privately for the query.
- **Test through the seam:** `WamrEngine.refusesAWamrThatCompiles` -- the linked WAMR says Absent; an injected
  `Present` is refused with `CompilesNativeCode`, the counting opener was never called, and the text is non-empty.
  `WamrEngine.copiesMemory` also checks `guestCode() == Interpreted`.

### 3. Packaging

- **Fedora** (`.github/fedora/contour.spec`): `%{_libexecdir}/contour/` added to the one `%files` section, outside the
  `%if %{without daemon}` block, so both the GUI and the daemon package ship it. Not run through rpmbuild/mock here.
- **Debian/Ubuntu** (CPack DEB, `src/contour/CMakeLists.txt`): no file list; CPack packages every install() -> the
  helper is included automatically. Nothing to change.
- **Windows** (CPack WIX/ZIP + `cmake/WindowsSignedOverlay.cmake`): CPack takes the install tree, but the signing
  manifest (`scripts/windows-signing/signers.psd1` `OwnBinaries = @('bin/contour.exe')`, and the SignPath
  `src/contour/packaging/signpath/payload.xml`) does not name `bin/contour-extension-host.exe`, and the overlay
  fails on any staged `.exe` the manifest lacks. **Not caused by this task**: Windows x86_64 has a pinned Wasmtime
  archive, so the helper was installed there before. Not changed (the SignPath configuration is pasted by the owner);
  needs a follow-up -- flagged in Concerns.
- **macOS bundle**: copies the helper into `Contents/Helpers` whenever the target exists (macOS has Wasmtime, so
  unchanged); `install()` is skipped for a bundle. No list.
- **FreeBSD**: no port or pkg-plist in the tree; the CI job only builds and tests. The external port's pkg-plist will
  need `libexec/contour/contour-extension-host` (new there: FreeBSD has no Wasmtime archive, so this task is what
  builds the helper on it).
- **Arch**: no PKGBUILD in the tree (AUR, external; makepkg has no file list).
- **Flatpak**: external manifest (`org.contourterminal.Contour.yml`, read from `origin/master` of the local Flathub
  checkout): no file list, and `cleanup` drops nothing under `/app/libexec`. Nothing to change in the tree.

### Commands and output (ASan tree unless noted)

```
cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_remote_test vtextension_wasmtime_test \
  vtextension_process_test vtextension_helper_test vtextension_wamr_helper_test contour-extension-host \
  contour-extension-host-wamr contour-extension-host-hostile extension_host_probe      -> exit 0, no warnings
vtextension_test "WamrEngine.*"                                   -> All tests passed (28 assertions in 4 test cases)
vtextension_test "WasmContract.clocksAnswerAlike*" (WAMR)         -> All tests passed (34 assertions in 1 test case)
vtextension_wasmtime_test "WasmContract.clocksAnswerAlike*"       -> All tests passed (34 assertions in 1 test case)
vtextension_remote_test "WasmContract.clocksAnswerAlike*"         -> All tests passed (102 assertions in 3 test cases)
vtextension_helper_test "WasmContract.clocksAnswerAlike*"         -> All tests passed (34 assertions in 1 test case)
vtextension_wamr_helper_test (whole)                              -> All tests passed (1237 assertions in 62 test cases)
vtextension_process_test "SeccompFilter.*"                        -> All tests passed (4017 assertions in 7 test cases)
ctest --preset=clang-asan -R "vtextension|containment"            -> 100% tests passed, 0 tests failed out of 8
clang++ ... cmake/WamrTerminateProbe.cpp libiwasm.a && ./wamr-probe -> exit 0
clang-format --dry-run (changed C++), editorconfig-checker (changed files) -> clean
```

### Concerns after the round

- **Windows signing manifest** lacks `contour-extension-host.exe` (pre-existing since the helper was first installed
  on Windows); a signed Windows package build would stop in `WindowsSignedOverlay.cmake`. Needs `OwnBinaries` and
  the SignPath payload updated by the owner.
- **Fedora** change is untested by rpmbuild here.
- **WAMR WASI now partly runs the shim** (two functions): deviates slightly from D98's "WAMR keeps its own WASI";
  the two engines now share those answers by construction.
- The flaky first-run concern from the first report stands (not seen again: every run in this round passed).
