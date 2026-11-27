### Task 5.2: The Linux sandbox — seccomp, no_new_privs and Landlock
**Files:**
- Create:
  - `host/Sandbox_linux.cpp`.
  - `host/linux/{SeccompFilter.hpp,SeccompFilter.cpp,SystemCallRows.hpp,SystemCallRows.cpp,Landlock.hpp,Landlock.cpp}`.
  - `host/linux/testing/BpfEvaluator.hpp` (a test-only classic-BPF interpreter for `seccomp_data`).
  - `host/linux/{SeccompFilter_test,Landlock_test}.cpp` (process_test, Linux).
  - `process/SandboxProbe_test.cpp` (process_test, Linux): real lockdown in a child.
- Modify:
  - `process/testing/LaunchProbe.cpp`: gains `--sandbox-op=<op>`, `--outer-filter=<kind>` and `--sandboxed` (with `--receive-grant`).
  - `transport/Channel_posix.cpp`: `refuseDescriptors` gets the errno table of D95, with a test in `Channel_test.cpp`.
  - `remote/testing/ProcessDefaultBackend.cpp` and `process/testing/HelperComposition.cpp`: `RequireLockedDown` where `ThisPlatformSandbox != None`.
  - `host/CMakeLists.txt` and `remote/CMakeLists.txt`: the probe links `vtextension_host`.

**Produces:**
- `SystemCallAction { Kill, Refuse, Allow }`. `Refuse` carries an errno.
- `ArgumentCheck{uint8_t index; ArgumentTest test; uint64_t mask; uint64_t value}`, where `ArgumentTest { Equals, AllClear, AllSet, EqualsControl, EqualsOwnProcess }`.
- `SystemCallRow{long number; std::span<ArgumentCheck const> checks; SystemCallAction action; int error; std::string_view reason}`.
- `releaseRows()`, `sanitizerRows()`, `coverageRows()`, and `buildRows()` (release + the set `ThisBuildInstrumentation` names).
- `assembleFilter(span<SystemCallRow const> rows, SandboxDescriptors, int ownProcess, AuditArch) -> expected<vector<sock_filter>, ConfinementFailure>`.
- `installFilter(span<sock_filter const>) -> expected<void, ConfinementFailure>`: `SECCOMP_SET_MODE_FILTER` with `SECCOMP_FILTER_FLAG_TSYNC`. A positive return (a thread id) → `ThreadSync`.
- `LandlockAbiRow{int abi; uint64_t fsRights; uint64_t netRights; uint64_t scopes; uint32_t restrictFlags}` and `landlockAbis()`. The constants are ours, because CI headers are older.
- `LandlockOutcome { Unavailable, Restricted }`.
- `restrictFileSystem(span<PathBeneathRule const> allowed, LandlockSystem const&) -> expected<LandlockOutcome, ConfinementFailure>`. `LandlockSystem` is three injectable function pointers (DI for the syscalls).
- `ThisPlatformSandbox : enum class PlatformSandbox : uint8_t { None, Linux, FreeBSD, MacOS, Windows }`.

Probe ops table (`--sandbox-op`). The op runs on an `ExecutorThread` started before lockdown, after a go signal:

| Op | Expect |
|---|---|
| `open-file` (`/etc/hostname`) | refused `EACCES` |
| `open-directory` (`/`) | refused `EACCES` |
| `socket-inet`, `socket-unix` | refused `EPERM` |
| `send-descriptor` (sendmsg + `SCM_RIGHTS` on control) | killed `SIGSYS` |
| `fork`, `spawn-thread`, `execve /bin/true`, `ptrace-traceme` | killed `SIGSYS` |
| `signal-parent` (`kill(getppid(), 0)`) | killed `SIGSYS` |
| `map-writable-executable` | killed `SIGSYS` |
| `map-shared-unfixed` | killed `SIGSYS` |
| `protect-writable-executable` | killed `SIGSYS` |
| `permitted` | `ok`. Runs channel send/recv, `recvmsg` of a grant, `mmap(MAP_SHARED\|MAP_FIXED)` over a reservation, `mprotect` RW, `munmap`, `close`, `getrandom`, `clock_gettime`, a condvar wait (futex), `sigaltstack`, and `membarrier` on aarch64 |

- [ ] **Step 1: Derive the allow-list by measurement** (D93).
  - Run `strace -f -qq -o <scratchpad>/<tree>.%p` over the unsandboxed helper while `vtextension_helper_test "~[.measure]"` runs in `out/clang-asan` and `out/clang-release`. Then repeat for `out/clang-tsan` and `out/clang-coverage` (one tree at a time).
  - Keep only the calls after each helper's `prctl(PR_SET_NO_NEW_PRIVS)` (Task 5.1 placed that marker).
  - Tabulate syscall × tree. The scripts stay in the scratchpad and are not committed.
  - Every syscall becomes a row with a reason, or is named in the report as refused-and-handled.
- [ ] **Step 2: Write the filter tests** (`BpfEvaluator`, no installation).
  - `"SeccompFilter.decidesEachRowAsItsTableSays"`, a table of `(arch, nr, args) → action`:
    - sendto/recvfrom/futex → Allow;
    - recvmsg on control → Allow, recvmsg on a lane → Kill;
    - sendmsg, sendmmsg → Kill (R38);
    - socket(AF_INET), socket(AF_UNIX) → Refuse EPERM;
    - openat → Refuse EACCES (release) and Allow (coverage set);
    - mmap `PROT_WRITE|PROT_EXEC` → Kill; mmap `MAP_SHARED` without `MAP_FIXED` → Kill; mmap `MAP_SHARED|MAP_FIXED` RW → Allow;
    - mprotect W+X → Kill;
    - clone, clone3, fork, vfork, execve, execveat, ptrace → Kill;
    - tgkill(own) → Allow, tgkill(other) → Kill;
    - a foreign arch → Kill; `__X32_SYSCALL_BIT` → Kill; nr 4095 → Kill.
  - `"SeccompFilter.noRowSetAllowsTheForbidden"`: no row in any of the three sets allows execve*, ptrace, process_vm_*, socket, connect, bind, sendmsg, sendmmsg, clone*, fork, vfork, unshare, setns, mount, bpf or perf_event_open.
  - `"SeccompFilter.everyRowSaysWhy"`: the reason is non-empty, with no duplicate number per set.
  - `"SeccompFilter.assemblesForBothArchitectures"`: x86_64 and aarch64 row numbers (aarch64 has no `open`, `fork` or `poll`). The program stays ≤ `BPF_MAXINSNS`.
- [ ] **Step 3: Write the Landlock tests** (fake `LandlockSystem`).
  - `"Landlock.abiTableIsMonotonic"`.
  - `"Landlock.usesTheBestAbiTheKernelHas"`: ABI 3 → no net/scopes, no TSYNC; ABI ≥ the TSYNC row → `RESTRICT_SELF_TSYNC`.
  - `"Landlock.unavailableIsNotAFailure"`: `ENOSYS`, `EOPNOTSUPP` → `Unavailable`.
  - `"Landlock.otherErrorsAreFailures"`: `EPERM` from restrict_self (no NNP) → `FileSystem`.
- [ ] **Step 4: Write the real lockdown tests** (process_test, Linux; the probe in a child).
  - `"Sandbox.forbiddenOperationsFailFromAnExecutor"`: over the ops table, each row's expectation (refused errno reported on control, or `ExitStatus{Signaled, SIGSYS}`).
  - `"Sandbox.permittedOperationsWorkFromAnExecutor"`.
  - `"Sandbox.landlockCoversEveryThread"`: the probe reads `/proc/self/status` `Seccomp:\t2` and `NoNewPrivs:\t1` for each `/proc/self/task/*` (opened before lockdown). `open-file` is refused from a thread started before restrict too, when the ABI has TSYNC.
  - `"Sandbox.locksDownWithoutLandlock"` (`--outer-filter=no-landlock`: an outer ALLOW filter returning `ENOSYS` for `landlock_*`) → `LockedDown`, and `open-file` is still refused (the seccomp row).
  - `"Sandbox.locksDownUnderAFlatpakLikeFilter"` (`--outer-filter=flatpak`: ALLOW default; `clone3` → `ENOSYS`; `unshare`, `setns`, `mount`, `personality`, `ptrace` → `EPERM`) → `LockedDown`, and the permitted op is `ok`.
  - `"Sandbox.threadSyncFailureFailsLockdown"`: a thread installs its own filter first → `ThreadSync`.
  - `"Channel.refusingDescriptorsMayBeUnavailable"` (outer filter: setsockopt `SO_PASSRIGHTS` → `EPERM`): `makeChannelPair()` succeeds; `upward == Discarded`.
- [ ] **Step 5: Write the helper case.** `"Helper.locksDown"` (helper_test, Linux): Hello `{LockedDown, None, 0}`. The binary's composition now requires `LockedDown`, so the whole helper_test runs sandboxed, including `everyAllowedWasiFunctionWorks` and `[bundled]`.
- [ ] **Step 6: Build** `vtextension_process_test extension_host_probe vtextension_remote_test vtextension_helper_test contour-extension-host`; expect failure.
- [ ] **Step 7: Implement the filter** (D91, D92).
  - Hand-assembled cBPF: arch check, the x32 bit, then the rows in frequency order, default `SECCOMP_RET_KILL_PROCESS`.
  - Argument checks use 32-bit halves (`seccomp_data.args` low/high words).
- [ ] **Step 8: Implement Landlock and the sandbox** (D94, D96).
  - `restrictBeforeThreads`: `PR_SET_NO_NEW_PRIVS`, then Landlock. Every handled FS right of the ABI, with no rules except the instrumented builds' (coverage object root write, sanitizer `/proc` read). Also net TCP bind/connect and the abstract-unix + signal scopes where the ABI has them.
  - `lockDown`: `buildRows()` → assemble → install with TSYNC. Unavailable Landlock tells once: `the file system restriction is unavailable on this system; system calls are restricted`.
- [ ] **Step 9: Implement the instrumented rows** (D96).
  - Sanitizer helper environment: `detect_leaks=0` appended to `ASAN_OPTIONS`/`LSAN_OPTIONS` (a row in the launcher's sanitizer environment table).
  - Coverage: the helper opens `CONTOUR_COVERAGE_OBJECT_ROOT` (a compile definition, the build tree) before Landlock and adds a `PATH_BENEATH` write rule.
  - `refuseDescriptors`: `EPERM`/`EACCES` → `Discarded` (D95).
- [ ] **Step 10: Run.**
  - `process_test "SeccompFilter.*" "Landlock.*" "Sandbox.*" "Channel.*"` → PASS.
  - `helper_test "~[.measure]"` whole → PASS, with `uncleanExits == 0` (a `SIGSYS` kill would show there).
  - Then the release tree: `cmake --build --preset clang-release -j 4 --target vtextension_helper_test contour-extension-host extension_host_probe vtextension_process_test`, and the same two runs. This is Signals mode, which is the product's row set.
  - ctest → PASS.
- [ ] **Step 11: Commit** (three):
  - `feat(vtextension): the extension host restricts its system calls on Linux`
  - `feat(vtextension): the extension host gives up file system access on Linux before it starts a thread`
  - `fix(vtextension): an outer sandbox refusing descriptor control no longer stops every extension host`


### Item 0 — carried from the previous task's review (do these FIRST)
1. **Before any seccomp filter relies on it:** run `WasmtimeEngine.prepareThreadRunsTheGuestPath` (src/vtextension/wasm/wasmtime/WasmtimeEngine_test.cpp ~1066-1071, the Signals section skipped under ASan and wine) in the release tree `out/clang-release`. It checks that priming installs Wasmtime's signal handlers and the thread's alternate signal stack, so no `rt_sigaction`/`sigaltstack` row is needed after lockdown. If it fails, the seccomp tables must carry those rows (or priming must be fixed) — report which.
2. The parent cannot recognise an older helper: a v3 Hello fails the strict decode (`Codec.cpp` ~210) before `refusalOf` checks the version (`HelperIncarnation.cpp` ~70). Read the protocol version before the rest of the Hello body and refuse a mismatch as `ProtocolVersion` (with its line). Test with a v3-shaped Hello.
3. The order test (`HostProcess_test.cpp` ~628-633) proves adopt happens before the sandbox is made, but not after hardening. Add a recording `adoptEndpoints` seam so the recorded order includes adopt.
One commit for 2+3 (`fix(vtextension): …`/`test(vtextension): …` as fits); item 1 produces a fact for "### Phase-5 facts", and a commit only if something must change.

### Controller notes — decisions and rulings that bind this task (verbatim from the plan)
- D91. Seccomp is hand-assembled classic BPF, with no libseccomp.
  - Arch check (x86_64, aarch64; anything else and the x32 bit → kill).
  - Rows from three tables (release, sanitizer, coverage) plus `SandboxDescriptors`, in frequency order.
  - Default `SECCOMP_RET_KILL_PROCESS`.
  - Tested through a test-only BPF evaluator.

  — No new dependency, and argument checks become unit-testable without killing a process.
- D92. Argument policy:
  - recvmsg only on the control descriptor;
  - sendmsg/sendmmsg denied entirely (the helper sends with `send`; R38);
  - socket/socketpair → `EPERM`; open/openat/openat2 → `EACCES` (soft refusal for library probes; Landlock is the second layer);
  - connect/bind/accept/listen killed;
  - mmap never `W|X`; `MAP_SHARED` only with `MAP_FIXED` and without `PROT_EXEC`; mprotect never `W|X` (W^X enforced);
  - clone*/fork/vfork/exec*/ptrace/process_vm_* killed;
  - kill/tgkill only on the own tgid;
  - read/write/poll are **not** descriptor-filtered.

  — After lockdown the helper can acquire no descriptor except those the parent grants, so a descriptor filter adds rows without adding a boundary.
- D93. The allow-list is measured, not guessed: strace of the unsandboxed helper across the helper suites in all four trees, from the no_new_privs marker on. It is re-audited at the gate against kernel seccomp audit records. — The engine's rare paths (traps, growth, compile) are what a guessed list misses.
- D94. Landlock:
  - uses the best ABI from our own constant table;
  - handles every FS right of that ABI with no rules (deny all);
  - denies TCP bind/connect (ABI ≥ 4) and scopes abstract-unix and signals (ABI ≥ 6);
  - uses `LANDLOCK_RESTRICT_SELF_TSYNC` where the ABI has it.
  - Unavailable (`ENOSYS`/`EOPNOTSUPP`) is **not** a failure: seccomp alone confines, and it is told once. Other errors → `FileSystem`.

  — Fedora, Ubuntu and Debian kernels differ, and Flatpak or an LSM list may lack it. Failing every extension there would push users to in-process builds.
- D95. Flatpak:
  - nested seccomp under bwrap's filter is fine (no_new_privs is already set);
  - Landlock follows D94;
  - the parent's `SO_PASSRIGHTS` treats `EPERM`/`EACCES` like `ENOPROTOOPT`/`EINVAL` (Discarded, not `Io`), because the helper's own filter now denies upward descriptors;
  - an outer filter is emulated by the probe.
  - A real in-Flatpak run is outstanding for Task 6.3: a host-built test binary does not run in the Platform runtime, and only Platform (no Sdk) is installed here.

  — This closes the 4.7 phase-input minor. The emulation covers the two mechanisms that differ.
- D96. Instrumented builds:
  - sanitizer helpers get `detect_leaks=0` (LSan's stop-the-world needs ptrace) and a Landlock read rule on `/proc`, so reports are unsymbolized (module+offset);
  - coverage helpers open the build tree's object root before Landlock and get a write rule plus the coverage seccomp rows;
  - D57's leak half moves to the in-process `HostServer` tests, as spec §5.6 says.

  — Every widening is compile-time and lives in its own row set.
- R38 (phase review; revises R36, extends D84). Parent ends set `SO_PASSRIGHTS = 0` where available (Linux ≥ 6.16): an upward `SCM_RIGHTS` fails in the helper with `EPERM`, so the parent never runs the last close of a helper's file (a lingering TCP socket could otherwise block a parent thread indefinitely). Phase 5 must add: helper seccomp denies `sendmsg`/`sendmmsg` carrying rights and `socket(AF_INET/AF_INET6)`; Seatbelt denies network and grants `ipc-posix-shm-read-data`/`write-data` only (no create); shm names retry on `EEXIST` with a random suffix.

- Path convention of this phase: paths are relative to `src/vtextension/wasm/` unless they start with `src/`, `cmake/`, `scripts/`, `support/`.
