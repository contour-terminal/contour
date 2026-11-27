# Task 5.2 report: the Linux sandbox (no_new_privs, Landlock, seccomp)

Status: DONE_WITH_CONCERNS (the concerns are at the end; none of them blocks 5.3 or 5.4).

Commits, on `feature/out-of-process-extension-host` from `8af97b4b3`, not pushed. Each one builds alone.

- `53548a8d7` fix(vtextension): refuse an older extension host for its version, not as a broken peer (Item 0.2 and 0.3)
- `4cb8b2af7` feat(vtextension): the extension host restricts its system calls on Linux
- `401b0dcc4` feat(vtextension): the extension host gives up file system access on Linux before it starts a thread
- `ee106c1cb` fix(vtextension): an outer sandbox refusing descriptor control no longer stops every extension host

## Item 0

1. **Priming check, release tree.** `vtextension_wasmtime_test "WasmtimeEngine.prepareThreadRunsTheGuestPath"`
   passes in `out/clang-release`, with 34 assertions in total. The Signals section passes alone with 18 assertions:
   `CHECK_FALSE(stackBefore)` and `CHECK(stackAfter)` both hold. Priming installs the thread's sigaltstack. The
   strace measurement agrees with this. `rt_sigaction` appears only between no_new_privs and lockdown, 3 times per
   helper (SEGV, ILL, FPE), and never after lockdown. The filter therefore has no `rt_sigaction` row, and a test
   pins that `rt_sigaction` is killed. Nothing in the code had to change.
2. **Older Hello.** `decodeBody<Hello>` reads the version first, using a separate reader. A Hello of another version
   decodes as `Hello{.version = v}` without reading the rest of its body, and `refusalOf` refuses it as
   `ProtocolVersion`. Tests:
   - `PduCodec.helloOfAnotherVersionDecodesItsVersion` sends a v3 layout, a longer future layout, and an empty body,
     which stays `Truncated`.
   - `HelperIncarnation.olderHostIsRefusedForItsVersion` has a fake launcher send raw v3 bytes. The test checks for
     exactly the `refusalLine(ProtocolVersion)`.
   - The lying-body cases and `helloAtItsLargestFitsItsCap` now use this protocol version.
3. **Adopt in the order test.** `HostConfinement` gains an `adoptEndpoints` seam (`systemConfinement()`: `&adoptEndpoints`).
   `HostProcess.locksDownInTheSandboxOrder` now records `bind, harden, adopt, engine, restrict, listener, …`.

## What was built

All of it lives in `src/vtextension/wasm/host/linux/`, together with `host/Sandbox_linux.cpp`.

- **`SystemCallRows.{hpp,cpp}`**
  - `AuditArch {Amd64, AArch64}` and `ThisAuditArch`.
  - `SystemCall` is an enum of every call that a row or a test names. Its numbers come from our own table, `systemCallNumbers()`, which also has the aarch64 numbers.
  - The spec's `SystemCallAction`, `ArgumentTest`, `ArgumentCheck` and `SystemCallRow`.
  - The row sets: `releaseRows`, `sanitizerRows`, `coverageRows`, `rowsFor(BuildInstrumentation, arch)` and `buildRows(arch)`. Each one takes an architecture.
  - The rows are three constexpr tables, and every row gives a reason.
- **`SeccompFilter.{hpp,cpp}`**
  - Hand-assembled cBPF. The program checks the architecture first, then rejects the x32 bit on x86_64, then walks the rows in frequency order. A call that no row names is killed with `SECCOMP_RET_KILL_PROCESS`.
  - A row whose number matches decides the call. If its checks pass, its action applies. If they fail, a `ret KILL` local to that row applies, which keeps every jump below 256.
  - Checks compare 32-bit halves.
  - There is an overload with the action for unlisted calls, which the probe's outer filters use.
  - `installFilter` uses TSYNC. A positive return becomes `ThreadSync` with code `EPERM`.
- **`testing/BpfEvaluator.hpp`**: a test-only cBPF interpreter over `seccomp_data`, plus `verdictOf()`.
- **`Landlock.{hpp,cpp}`**
  - Our own ABI table, rows 1 to 10. TSYNC arrives at ABI 8 (the kernel documentation lists "Thread synchronization (ABI < 8)").
  - `LandlockSystem` holds 4 injectable function pointers. Each returns its result, or `-errno`.
  - `restrictFileSystem()` returns `Unavailable` on `ENOSYS` or `EOPNOTSUPP`, and `FileSystem` on any other error.
  - The ruleset handles every right of the ABI and has no rules apart from the instrumented builds'.
- **`Sandbox_linux.cpp`**
  - `restrictBeforeThreads`: no_new_privs, then opens the instrumented build's directories (`O_PATH`), then Landlock. Every step runs, and the first failure is the one stated. When Landlock is unavailable, the host tells this line once: `the file system restriction is unavailable on this system; system calls are restricted`.
  - `lockDown`: `buildRows`, then assembly, then install with TSYNC.
  - `LockedDown`.
- **`Sandbox.hpp`**: `PlatformSandbox` and `ThisPlatformSandbox`. The value comes from `CONTOUR_PLATFORM_SANDBOX`, a PUBLIC definition on `vtextension_host`.
- **CMake**
  - `VTEXTENSION_LINUX_SANDBOX` is set in `src/vtextension/CMakeLists.txt`, to Linux on x86_64 or aarch64. It is read by `host/` (sources and value) and `remote/` (the probe's sandbox mode and the process_test sources). It has to be a variable, because `host/` is added after `remote/`.
  - `CONTOUR_COVERAGE_OBJECT_ROOT` (`CMAKE_BINARY_DIR`) is defined only under `CONTOUR_COVERAGE`, so no build path is compiled into a release binary.
- **Probe**
  - `process/testing/SandboxProbe.{hpp,cpp}`. LaunchProbe dispatches to it when `--receive-grant` comes with `--sandboxed`, `--outer-filter=` or `--sandbox-op=`.
  - The ops are a table and the outer filters are a table: `no-landlock`, `flatpak`, `passrights` (setsockopt SO_PASSRIGHTS only) and `thread`.
  - The lockdown report goes out on lane 0 and the op report on lane 1.
- **Compositions**
  - `HelperComposition.hpp`: `HelperLockdown`, which is `RequireLockedDown` wherever a sandbox exists. It is the default of `HelperBackendConfig`, and `ProcessDefaultBackend` uses it. The whole helper_test therefore runs sandboxed, `[bundled]` and the WASI contract included.
  - `receiveHello` expects `LockedDown` on Linux.
  - `Process.unsandboxedIsRefusedUnlessAccepted` now checks that a locked-down helper is driven under `RequireLockedDown`.
- **Sanitizer environment**: the table moved to `LaunchedHelper.{hpp,cpp}` (`sanitizerEnvironment()`), so process_test can use it as well. In a sanitizer build, `detect_leaks=0` is appended to `ASAN_OPTIONS` and `LSAN_OPTIONS`, whether those variables are set or not.
- **D95**: `transport::upwardDescriptorsWhenUnrefused(errno)` holds the errno table. `ENOPROTOOPT`, `EINVAL`, `EPERM` and `EACCES` mean `Discarded`; anything else is `Io`.

## Measured allow-list (D93)

- **Command.** `strace -ff -qq -ttt -o <scratchpad>/measure/<tree>/t vtextension_helper_test "~[.measure]"`, run
  over a temporary measuring sandbox. Its `restrictBeforeThreads` called `prctl(PR_SET_NO_NEW_PRIVS)` and its
  `lockDown` called `prctl(PR_GET_NO_NEW_PRIVS)` as a marker. That sandbox was not committed.
- **Analysis.** A script groups threads by helper (following `clone3` returns) and counts the calls made after the
  marker. The scripts are in the scratchpad (`analyze.py`, `analyze2.py`) and are not committed. Task 5.1 had not in
  fact placed a no_new_privs marker, which is why the temporary one was added.
- **Helpers counted.** 182 in release, 181 in ASan, 181 in TSan and 182 in coverage.

Calls after lockdown, by tree:

| syscall | release | asan | tsan | coverage | row |
|---|---|---|---|---|---|
| write | 177 | 704 | 176 | 2876785 | allow |
| mmap | 877 | 21744 | 9223 | 6104 | allow: no PROT_EXEC; shared only with MAP_FIXED (coverage: shared, unfixed) |
| shutdown | 5311 | 5281 | 5282 | 5312 | allow |
| madvise | 889 | 6484 | 9418 | 891 | allow |
| munmap | 2023 | 3926 | 4022 | 7250 | allow |
| fcntl | 0 | 0 | 0 | 15399 | coverage |
| clock_gettime | 0 | 0 | 12855 | 0 | allow (the release row is the vDSO fallback) |
| close | 1850 | 1839 | 1839 | 6983 | allow |
| sendto | 3072 | 3071 | 3071 | 3072 | allow |
| getpid | 0 | 1408 | 0 | 10266 | allow (abort) |
| fstat | 255 | 255 | 255 | 10521 | allow; newfstatat/statx with AT_EMPTY_PATH too |
| recvfrom | 2473 | 2473 | 2503 | 2473 | allow |
| rt_sigprocmask | 885 | 2992 | 880 | 885 | allow |
| openat | 2 | 0 | 0 | 5138 | refused EACCES; sanitizer and coverage allow it |
| read | 2 | 0 | 0 | 5132 | allow |
| lseek | 0 | 0 | 0 | 5133 | coverage |
| recvmsg | 1160 | 1159 | 1163 | 1160 | allow on control only (all measured calls were fd 3) |
| sigaltstack | 885 | 1760 | 880 | 885 | allow |
| futex | 86 | 430 | 3236 | 191 | allow |
| brk | 1765 | 0 | 0 | 2024 | allow |
| exit | 885 | 880 | 880 | 885 | allow |
| mprotect | 623 | 792 | 616 | 619 | allow unless W+X (none measured) |
| sched_yield | 0 | 1088 | 0 | 0 | allow |
| exit_group | 177 | 352 | 176 | 177 | allow |
| ioctl | 177 | 177 | 177 | 177 | refused ENOTTY (`TCGETS2` on fd 2 at every CreateRuntime) |
| prctl, clone, wait4, ptrace | 0 | 704/352/352/176 | 0 | 0 | none: LSan at exit, gone with detect_leaks=0 |
| gettid | 0 | 352 | 0 | 0 | allow |
| mremap | 71 | 0 | 0 | 81 | allow |
| gettimeofday, nanosleep | 0 | 0 | 50/50 | 0 | sanitizer |
| poll | 7 | 7 | 7 | 7 | allow (ppoll for aarch64) |
| rt_sigreturn | 9 | 0 | 0 | 0 | allow (Signals traps) |
| restart_syscall | 1 | 1 | 2 | 1 | allow (resuming after SIGSTOP) |

- **Refused and handled.**
  - `openat` plus `read` of `/proc/sys/vm/overcommit_memory`. I resolved the path from libc's `.rodata`. This is glibc's
    `check_may_shrink_heap`: when the open fails, glibc does not shrink the heap.
  - `ioctl(TCGETS2)`, which Wasmtime's isatty probe makes. `ENOTTY` is the same answer the helper gets today.
- **Rows that were not measured.**
  - `getrandom`, `clock_gettime` and the aarch64 `membarrier` come from the brief's permitted op.
  - `newfstatat`/`statx` cover aarch64 glibc's `fstat`. This is D109a.
  - `tgkill`/`kill` on the own process, and `getpid`/`gettid` in release, are for abort.
  - `ftruncate` is in the coverage rows, from GCDAProfiling's source: it sizes a counter file that is new to the tree.
- **Checked after lockdown, in every tree.** No mmap with `PROT_EXEC`, no `mprotect` W+X, and no `recvmsg` on a
  descriptor other than control. Unfixed shared mmaps appear only in coverage (5130).

## Deviations, with reasons

1. **The `ArgumentTest::NotEquals` enumerator was added.** With one row per number per set, "shared implies fixed"
   and "not W and X together" cannot be expressed with the spec's five tests.
2. **mmap forbids `PROT_EXEC` outright,** which is stricter than D92's "never W|X". Nothing maps executable memory
   after lockdown, as measured above: Wasmtime maps RW and then mprotects RX. The rule also covers D92's
   "shared, not exec" without needing a cross-argument check.
3. **`LandlockSystem` has 4 pointers.** I added `closeRuleset`, so that a fake never closes a real descriptor.
4. **The brief's "the probe reads `/proc/self/task/*/status`" is done by the parent.** The parent reads
   `/proc/<pid>/task/*/status`. Landlock is applied in `restrictBeforeThreads`, before the executor thread exists,
   so the probe cannot open that thread's status file after restrict. The non-dumpable probe's status files stay
   readable to its parent. The "open-file from a thread started before restrict" check runs between restrict and
   lockdown. This isolates Landlock from the seccomp row, and it is expected to be refused only when ABI ≥ 8.
5. **`AuditArch::Amd64`, not `X86_64`.** clang-tidy's naming rule rejects `X86_64` and the `x86_64` member.
6. **The probe's sandbox mode lives in `SandboxProbe.{hpp,cpp}`, not inline in `LaunchProbe.cpp`,** to keep the
   file readable. LaunchProbe only dispatches to it.
7. **Some tests sit where the code is rather than where the brief put them.**
   - `Channel.refusingDescriptorsMayBeUnavailable` runs in process_test through the probe (`--outer-filter=passrights`, op `channel-pair`), so the test process is never filtered.
   - The pure errno table is tested in `Channel_test.cpp` (`Channel.unrefusableDescriptorsAreDiscarded`).
8. **The test expectations follow the build.**
   - In instrumented builds the instrumented rows allow `openat`, and coverage builds allow `mmap` shared unfixed. So `Sandbox.locksDownWithoutLandlock` expects `op=ok` there, and the `map-shared-unfixed` row expects `ok` in a coverage build.
   - Under Landlock (the normal case), `open-file` is refused in every tree.
9. **`Sandbox.systemWithoutOneRunsUnsandboxed` (5.1) now SKIPs** where a sandbox exists. It called
   `makeSystemSandbox()` in the test process itself, which locked down `vtextension_remote_test`, and a later case
   died with SIGSYS. The fix is folded into `4cb8b2af7`.
10. **The Landlock table goes to ABI 10.** It also handles RESOLVE_UNIX (ABI 9) and UDP bind/connect (ABI 10), which
    is beyond D94's TCP-only rule. This is "every right the ABI knows", and the socket rows refuse those calls anyway.

## Tests and commands (all run; real counts)

- Item 0.1, release:
  - `vtextension_wasmtime_test "WasmtimeEngine.prepareThreadRunsTheGuestPath"`: 34/34 assertions.
  - `-c "signal traps"`: 18/18 assertions.
- **ASan, final head (`ee106c1cb`).**
  - `ctest --preset=clang-asan -R vtextension`: 5/5 passed.
  - `vtextension_process_test`, whole: 124 cases (123 passed, 1 skipped), 4355 assertions.
  - `vtextension_process_test "SeccompFilter.*,Landlock.*,Sandbox.*,Channel.*"`: 17 cases, 3509 assertions, all passed.
  - `vtextension_helper_test "~[.measure]"`: 159 cases (158 passed, 1 skipped for caps), 3872 assertions. `UncleanHelperExits` reported nothing, so `uncleanExits` was 0 in every case, and `Helper.locksDown` is among them.
- **Release** (`cmake --build --preset clang-release -j 4 --target vtextension_helper_test contour-extension-host extension_host_probe vtextension_process_test contour-extension-host-hostile`):
  - The process_test filter above: 17 cases, 3509 assertions, passed.
  - helper_test `~[.measure]`: 159 cases, 3885 assertions, passed (Signals mode, the product's rows).
- **TSan** (`out/clang-tsan`):
  - The process_test filter: 17 cases, 3511 assertions, passed.
  - helper_test: 159 cases (158 passed, 1 skipped), with no ThreadSanitizer report.
- **Coverage** (`out/clang-coverage`):
  - The process_test filter: 17 cases, 3511 assertions, passed.
  - helper_test: 159 cases, 3885 assertions, passed, with no `profiling:` error: sandboxed helpers created and wrote their `.gcda` files under Landlock.
  - These runs only pass after `find out/clang-coverage -name '*.gcda' -delete`. Without it, the existing corrupt counter files made the profile runtime segfault at exit in every process, the test binary included.
- **Intermediate commits.** `4cb8b2af7` and `401b0dcc4` were each checked out and built with
  `-j4 --target vtextension_process_test extension_host_probe vtextension_helper_test vtextension_test vtextension_remote_test vtextension_wasmtime_test contour-extension-host contour-extension-host-hostile`.
  Both builds were clean. `53548a8d7` was built and ctest'ed: 5/5 passed.
- **clang-tidy.** `JOBS=4 scripts/clang-tidy-diff.sh --base 8af97b4b3`: 24 translation units clean. Earlier findings were fixed rather than suppressed: naming, member init, nested conditionals, and the analyzer's MmapWriteExec. The probe now maps W+X through `syscall(SYS_mmap)`, which is the call under test.
- **Format and editorconfig.** `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` are clean over the branch diff.
- **Not done: `/simplify`.** It was not run as a skill, because the rules forbid subagents. I did a manual simplification pass instead: one sanitizer environment table, shared tables for ops and outer filters, `rowsFor()` reused by both `buildRows` and the tests.

## Concerns

1. **Without Landlock, an instrumented helper can open files.** The sanitizer and coverage rows allow `openat` (D96).
   This affects only instrumented builds on kernels without Landlock. Release is unaffected.
2. **A sanitizer-built Contour starting helpers outside the tests gets no `detect_leaks=0`.** Production's
   `defaultHelperEnvironment()` is empty, so such a helper would die by SIGSYS at exit, when LSan runs. This is a
   developer-only scenario, but it starts to matter once 5.4 makes out-of-process the default. One fix is a
   `__lsan_default_options` hook in the helper binary, or the same row in `defaultHelperEnvironment()` for
   sanitizer builds.
3. **The probe's outer filters inherit the assembler's semantics.** A row whose checks fail kills the call, even
   in an Allow-default filter. The `passrights` row checks `setsockopt` for `SO_PASSRIGHTS` only, so any other
   `setsockopt` would kill the probe. Linux's channel code makes no other `setsockopt` call, so the tests are
   unaffected. A real Flatpak filter is not emulated beyond the listed calls, and D95's in-Flatpak run stays
   outstanding for 6.3.
4. **arm64 is proven by reading and the evaluator only.** The aarch64 numbers come from asm-generic/unistd.h on
   this machine, and the `membarrier` row is untested on hardware. TSYNC is ABI 8, read from the kernel
   documentation; this kernel reports ABI 10.
5. **The coverage tree's counter files get corrupted.** The most likely cause is helpers that the supervisor kills
   mid-write. The corruption existed before this task, but it now makes coverage runs flaky until the `.gcda` files
   are deleted. A gate run should delete them first.
6. **The probe's two-reports-on-one-lane race** showed up in the coverage tree only. It is fixed by sending each
   report on its own lane. `readReport` itself may still read past an `end` line when two reports share a lane,
   and the other users of `readReport` send one report per lane.

## Fix round 1

These are two new commits on top of `ee106c1cb`; neither is folded into an earlier commit. Each one was built at its own head.

- `b45c709c7` fix(vtextension): a sanitizer build's extension host no longer dies by its own filter as it exits
- `f715e00fd` fix(vtextension): a kernel refusing what our Landlock row claims for its ABI falls back to the row before

### Important 1: detect_leaks=0 at compile time

- `src/vtextension/wasm/host/SanitizerOptions.cpp` defines `__asan_default_options` and `__lsan_default_options`. Both return `detect_leaks=0`, and only when `CONTOUR_BUILD_INSTRUMENTATION == 2`.
  - The C++ names are `asanDefaultOptions` and `lsanDefaultOptions`. An `__asm__` label gives them the runtime's C symbol names, which keeps clang-tidy's camelBack rule satisfied without a NOLINT.
- These functions are built into the `OBJECT` library `vtextension_host_sanitizer_options`. Two executables link it:
  - `contour-extension-host`;
  - `extension_host_probe`, wherever the Linux sandbox is built.
- It has to be an object library. In a static library, nothing would pull this member in, because the sanitizer runtime already provides weak defaults for both functions.
- The unsandboxed hostile host does not link it, so it keeps leak detection.
- `nm` shows `T __asan_default_options` and `T __lsan_default_options` in the ASan helper, and none in the release helper. In the ASan helper, `__ubsan_default_options` and `__sancov_default_options` remain the runtime's own weak defaults (`W`).
- The appended row in the test environment table is gone. `sanitizerEnvironment()` now only passes this process's variables through.
- Covering test: `Helper.exitsCleanlyWithAnEmptyEnvironment` (helper_test). It starts a helper with an empty environment, expects `Hello{LockedDown}`, closes control, and expects `Exited 0`.
  - It passes: 11 assertions.
  - To show the hazard the test guards against, I ran `ASAN_OPTIONS=detect_leaks=1 vtextension_helper_test "Helper.saysHelloOverInheritedChannels"`. The exit check fails there (`{?} == {?}`; 10 passed, 1 failed), because the helper is killed at exit.

### ⚠️ 2: the Landlock ABI 8–10 constants

What I checked them against:

- `/usr/include/linux/landlock.h` from `kernel-headers-7.2.4-200.fc44`, and `/usr/src/kernels/7.2.13-200.fc44.x86_64/include/uapi/linux/landlock.h`, which is identical on these lines. The header gives:
  - `LANDLOCK_RESTRICT_SELF_TSYNC (1U << 3)`;
  - `LANDLOCK_ACCESS_FS_RESOLVE_UNIX (1ULL << 16)`, "available since the ninth version of the Landlock ABI";
  - `LANDLOCK_ACCESS_NET_BIND_UDP (1ULL << 2)` and `LANDLOCK_ACCESS_NET_CONNECT_SEND_UDP (1ULL << 3)`, "Support added in Landlock ABI version 10";
  - REFER at version 2, TRUNCATE at 3, the TCP rights at 4, IOCTL_DEV at 5, and the scopes at 6.
- The header does not say which ABI version TSYNC belongs to. For that I used the upstream `Documentation/userspace-api/landlock.rst`. The only copy here is in the scratchpad (`scratchpad/wt/landlock.rst`): it is not installed by any package. Its sections read "Thread synchronization (ABI < 8)" and "Starting with the Landlock ABI version 8 … `LANDLOCK_RESTRICT_SELF_TSYNC`".
- This kernel reports ABI 10.

The new test `Landlock.abiTableAgreesWithTheSystemHeaders` pins the table against the header. For each ABI, it compares what the table adds with the header's constant, wherever the header defines that constant. This covers ABIs 1–6 and 8–10. ABI 7 adds no rights or flags the table uses. On an older CI header, the comparisons it cannot make compile out.

The fallback:

- `restrictFileSystem` now walks the rows from the best one the kernel's ABI allows down to row 1, through `enforceAbi()`. An `EINVAL` from any step moves on to the next lower row: creating the ruleset, adding a rule, or restricting. Every ruleset that was created is closed.
- Any other error still fails the lockdown at once.
- If every row is refused, the result is `FileSystem` with `EINVAL`.
- Test: `Landlock.aKernelRefusingAnAbiRowFallsBackToTheOneBefore`.
  - A fake reports ABI 10 but rejects the UDP rights. The result is `Restricted` at row 9, after 2 attempts.
  - A fake rejects the TSYNC flag at restrict time. The result is row 7, with 4 rulesets closed.
  - A fake rejects row 1's file system rights at ABI 5. The result is `FileSystem`/`EINVAL` after 5 attempts.
  - `ENOMEM` gets no retry: 1 attempt.

### Commands and results

- `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_wasmtime_test vtextension_remote_test vtextension_helper_test vtextension_process_test extension_host_probe contour-extension-host contour-extension-host-hostile`: clean, with no warnings.
- `vtextension_helper_test "Helper.*"` at `b45c709c7`: 66 assertions, passed.
- `vtextension_process_test "[sandbox]"` at `b45c709c7`: 16 cases, 3498 assertions, passed.
- `vtextension_process_test "Landlock.*"`: 6 cases, 132 assertions, passed.
- `vtextension_process_test "SeccompFilter.*,Landlock.*,Sandbox.*,Channel.*"` at `f715e00fd`: 19 cases, 3539 assertions, passed.
- `ctest --preset=clang-asan -R vtextension` at `f715e00fd`: 5/5 passed, in 41.8 s.
- Release build of `contour-extension-host` and `extension_host_probe`: clean. `nm | grep -c default_options` gives 0.
- `scripts/clang-tidy-diff.sh --base HEAD`: clean for each fix (4 translation units, then 2). clang-format and editorconfig-checker are clean.

### Remaining concerns

- Nothing here was run on a kernel with an ABI between 1 and 9. The fallback and the header comparison make a wrong row cost that ABI's additions rather than every extension, but they do not prove the rows right.
- An explicit `ASAN_OPTIONS=detect_leaks=1` in the environment still overrides the compile-time default, and the helper then dies by SIGSYS at exit. That takes a deliberate setting, and the default is now safe.

## Fix round 2

Finding: `Helper.exitsCleanlyWithAnEmptyEnvironment` set `launch.environment = {}`. On Windows Contour does not start
the helper that way: it passes `defaultHelperEnvironment()`, which there is `{SystemRoot}`. Without `SystemRoot` the
helper cannot load the system DLLs, so the test fails before Hello.

Fix: the test now sets `launch.environment = process::defaultHelperEnvironment();`. That is the environment Contour
uses, and it still leaves out the test process's sanitizer variables, so the test still covers the compile-time hook.
On Linux the list is empty, so nothing changes there.

I folded the change into the commit that added the test, with a fixup commit and
`GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash ee106c1cb`. The new hashes:

- `9e97cfca9` fix(vtextension): a sanitizer build's extension host no longer dies by its own filter as it exits
  (was `b45c709c7`)
- `67e99c3d9` fix(vtextension): a kernel refusing what our Landlock row claims for its ABI falls back to the row
  before (was `f715e00fd`)

Commands and results:

- ASan, building each commit at its own head:
  `flock -o … nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_helper_test vtextension_process_test extension_host_probe contour-extension-host contour-extension-host-hostile`
  - `9e97cfca9`: clean. `vtextension_helper_test "Helper.exitsCleanlyWithAnEmptyEnvironment"` passed, 11 assertions.
  - `67e99c3d9`: clean. The same test passed, 11 assertions.
- Windows cross-build (`out/clangcl-debug`, clang-cl with the MSVC kits in `/opt/msvc`, `-Werror`):
  `cmake --build out/clangcl-debug -j 4 --target vtextension_helper_test contour-extension-host contour-extension-host-hostile extension_host_probe`.
  Clean at `67e99c3d9`. This also cross-compiles the rest of the branch for Windows: `SanitizerOptions.cpp`, which is
  inert outside sanitizer builds, `Sandbox.hpp`'s `PlatformSandbox`, and the Channel changes.
- wine 11.20, with a prefix of my own (`scratchpad/wine52`) and `WINEPATH` set to mingw's `zlib1.dll`:
  `wine out/clangcl-debug/bin/vtextension_helper_test.exe "Helper.exitsCleanlyWithAnEmptyEnvironment,Helper.saysHelloOverInheritedChannels"`
  passed: 2 test cases, 22 assertions. Afterwards I ran `wineserver -k` on that prefix only.
