# Task 5.5: FreeBSD sandbox, report

Status: DONE_WITH_CONCERNS. The code is complete and checked locally: it builds and its tests pass on Linux, and
every FreeBSD path passes a syntax check against real FreeBSD headers. It has never run on FreeBSD, so it is
proven only by reading. Not pushed.

## Commits

- `b7e78b8c9` refactor(vtextension): the extension host names every descriptor it keeps, and what for
  - The neutral rights table, `SandboxDescriptors::held`, `IHostedEngine::heldDevices()`,
    `standardErrorHandle()`, `sandboxDescriptorsOf()`, and Linux passing the descriptors by reference.
  - Built on its own: the vtextension test targets, the probe and the hostile helper. Its `Sandbox.*`,
    `HostProcess.*` and `WamrEngine.*` tests passed.
- `5edd959d9` feat(vtextension): the extension host enters capability mode on FreeBSD
  - The FreeBSD sandbox, its CMake, the caps, the probe port and the FreeBSD expectations.

## What changed

### Platform-neutral, built and tested everywhere
- `host/DescriptorRights.{hpp,cpp}` (new):
  - `DescriptorRole { Control, Lane, Page, Wake, StandardError, EngineDevice }`.
  - `DescriptorRight { Read, Write, Receive, Send, Event, Shutdown, Fstat, MapRead, Seek, GetStatusFlags }`.
    There is deliberately no right for ioctl.
  - The rights table, one row per role at its enumerator's index, checked by `static_assert`.
  - `descriptorRoleRights()` and `rightsOf(role)`.
- `Sandbox.hpp`: `SandboxDescriptors` gains `std::vector<HeldDescriptor> held` (a role and a handle per entry,
  with a default), beside `control`. The Linux filter still reads only `control`.
- `IHostedEngine::heldDevices()` (new, pure virtual): the devices an engine opened before the lockdown and
  keeps using after it.
  - The WAMR engine returns its shared null device. `NullDevice::sharedHandle()` is new.
  - Wasmtime returns none.
  - `DecoratedEngine` and `PreparationHookEngine` forward the call.
- `standardErrorHandle()` (HostBootstrap, per system) and `sandboxDescriptorsOf(endpoints, engine)`
  (HostProcess). The host now hands its sandbox control, each lane, the page, the wake end, stderr and the
  engine's devices.
- The Linux code paths keep their behaviour. The only Linux changes are these:
  - `assembleFilter` and `comparisonOf` take `SandboxDescriptors const&`, because the struct is no longer
    trivially copyable (clang-tidy `performance-unnecessary-value-param`).
  - `LinuxSandbox` moves its descriptors instead of copying them.

### FreeBSD
- `host/Sandbox_freebsd.cpp` (new):
  - `restrictBeforeThreads()`: `procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, &PROC_NO_NEW_PRIVS_ENABLE)` under
    `#ifdef PROC_NO_NEW_PRIVS_CTL`, and nothing on older systems.
  - `lockDown()` takes three steps, and runs every step even after one fails. The first failure is the one
    reported.
    1. It limits control and every held descriptor: `cap_rights_init`, then `cap_rights_set` once per right
       through a `DescriptorRight → CAP_*` table, then `cap_rights_limit`.
    2. It calls `cap_fcntls_limit(fd, CAP_FCNTL_GETFL)` where the role has `GetStatusFlags`.
    3. It calls `cap_enter()`.
  - A failed step is reported as `DescriptorRights` or `Capabilities` (LockdownStep values that already
    existed).
- `HostBootstrap_posix.cpp` changes the FreeBSD caps to `RLIMIT_NPROC = 1` (a new `CappedValue::One`) and
  `RLIMIT_AS = dataBytes`.
- CMake:
  - `src/vtextension/CMakeLists.txt` adds `VTEXTENSION_FREEBSD_SANDBOX`, which is ON whenever
    `CMAKE_SYSTEM_NAME` is FreeBSD.
  - `host/CMakeLists.txt` then builds `Sandbox_freebsd.cpp` with `CONTOUR_PLATFORM_SANDBOX=2`.
  - `remote/CMakeLists.txt` builds the sandbox probe and `SandboxProbe_test.cpp` there too.
  - `_CONTOUR_SANDBOXED_PLATFORMS` is unchanged, as the controller ruled. FreeBSD keeps in-process
    extensions by default.
- The probe (`SandboxProbe.cpp`) is ported to FreeBSD:
  - The outer filters (seccomp) are Linux-only.
  - `ptrace` uses `PT_TRACE_ME`.
  - `mmap` is called directly, because FreeBSD's `syscall()` returns an `int`.
  - `membarrier` is Linux-only.
  - New ops: `connect-inet`, `ioctl-control` and `pdfork` (FreeBSD only).
  - The probe now hands the sandbox its lanes and stderr as held descriptors.
  - When sandboxed it calls `capResources` before `lockDown()`, as the host does.
- `LaunchProbe.cpp` reports `as=`, and its sandbox mode is built for `CONTOUR_PLATFORM_SANDBOX == 2` too.

### Rights per role (the table)
| role | rights | Capsicum |
|---|---|---|
| Control | Read Write Receive Send Event Shutdown Fstat GetStatusFlags | READ WRITE RECV SEND EVENT SHUTDOWN FSTAT FCNTL(GETFL) |
| Lane | the same without Fstat | READ WRITE RECV SEND EVENT SHUTDOWN FCNTL(GETFL) |
| Page | MapRead Fstat GetStatusFlags | MMAP_R FSTAT FCNTL(GETFL) |
| Wake | Read Event GetStatusFlags | READ EVENT FCNTL(GETFL) |
| StandardError | Write Fstat GetStatusFlags | WRITE FSTAT FCNTL(GETFL) |
| EngineDevice | Write Fstat Seek GetStatusFlags | WRITE FSTAT SEEK FCNTL(GETFL) |

No role gets `CAP_IOCTL`, so `cap_rights_limit` empties each descriptor's ioctl list (sys_capability.c, below).

## Tests
- `Sandbox.rightsTableCoversEveryRole` (vtextension_remote_test) runs on Linux. It checks:
  - one row per role, at its index;
  - each row's rights are non-empty and free of duplicates;
  - `rightsOf()` agrees with the table;
  - the rights each role needs are present, and the page and stderr cannot write and read respectively.
- `WamrEngine.holdsTheNullDeviceItOpened` (vtextension_test): the engine's held device is the handle the
  opener returned.
- The host test's `helloOf` now checks the full held list:
  - lanes, page, wake and stderr, with their roles;
  - nothing from Wasmtime.
- `SandboxProbe_test.cpp`: `Sandbox.forbiddenOperationsFailFromAnExecutor` is now driven by one expectation
  table per system, using a `Fate` of Allowed, Refused, Killed or Unasserted.
  - The Linux table is the old one, plus `connect-inet → EPERM`.
  - The FreeBSD table:
    - `open-file`, `open-directory`, `open-own-process`, `open-other-process` → `ECAPMODE`;
    - `socket-inet`, `socket-unix` → allowed (see the deviations);
    - `connect-inet` → `ECAPMODE`;
    - `ioctl-control` → `ENOTCAPABLE`;
    - `send-descriptor` → Unasserted (logged with `WARN`);
    - `fork` and `pdfork` → `EAGAIN`, unless the caps are skipped (sanitizer build) or the test runs as root,
      who holds `PRIV_PROC_LIMIT`;
    - `execve`, `ptrace-traceme`, `signal-parent` → `ECAPMODE`.
  - `Sandbox.permittedOperationsWorkFromAnExecutor` runs on both systems; the Flatpak-like outer filter runs
    on Linux only.
  - The Landlock, W^X, outer-filter and `SO_PASSRIGHTS` tests are Linux-only.
- `HostBootstrap.capsHold` on FreeBSD expects `nproc=1` and `as=<ProbeDataBytes>`.

## Step 4 checklist (verified by reading; sources are FreeBSD releng/14.2 unless noted)
1. **`cap_rights_init` variadic use.**
   - `sys/sys/capsicum.h:324-329`: `#define cap_rights_init(...) __cap_rights_init(CAP_RIGHTS_VERSION, __VA_ARGS__, 0ULL)`,
     and `cap_rights_set(...)` is `__cap_rights_set(__VA_ARGS__, 0ULL)`. The header itself appends the
     terminating 0, so `cap_rights_init(&r)` followed by one `cap_rights_set(&r, right)` per right is
     well-formed.
   - cap_rights_init(3): "Capability rights should be separated with comma"; library `libc`.
   - `CAP_*` values are `CAPRIGHT(idx, bit)` `unsigned long long` constants (capsicum.h:75-241), so a
     `constexpr` table of `uint64_t` holds them with no narrowing: they are constants that fit.
   - `CAP_RECV == CAP_READ` and `CAP_SEND == CAP_WRITE` (capsicum.h:188-189).
   - `CAP_MMAP_R = CAP_MMAP|CAP_SEEK|CAP_READ` (capsicum.h:92).
   - `CAP_FCNTL_GETFL = (1 << F_GETFL)` (capsicum.h:313).
2. **`<sys/capsicum.h>` is enough, and libcasper is not needed.**
   - cap_enter(2), cap_rights_limit(2), cap_fcntls_limit(2) and cap_rights_init(3) all list "Standard C
     Library (libc, -lc)".
   - Nothing the host does needs a Casper service. It opens nothing after the lockdown.
3. **`cap_enter` comes after every thread.**
   - `lockDown()` runs from `HostServerConfig::confine`, which HostServer calls once every executor and the
     listener are started and prepared. That is unchanged from Linux, and the host test's step order checks
     it.
   - `sys_cap_enter` sets `CRED_FLAG_CAPMODE` on a new process credential (`sys_capability.c:108-113`,
     `proc_set_cred`).
   - Every thread takes the new credential at its next system call (`subr_syscall.c:68-69`,
     `td_cowgen != p_cowgen` → `thread_cow_update`), so threads that already run are covered too.
   - `procctl` is **not** CAPENABLED (`syscalls.master` 544 is `STD`), so `PROC_NO_NEW_PRIVS_CTL` has to come
     before `cap_enter`. It does: it runs in `restrictBeforeThreads()`.
4. **WAMR's interpreter needs no new descriptor.**
   - The null device is opened once in `makeWamrEngine`, before the lockdown (task 5.3).
   - With the strace table of task 5.3 and WAMR 2.4.5's `posix.c` and `posix_file.c` read again, the device
     sees four kinds of call after the lockdown:
     - `fstat`, at instantiation;
     - `isatty` → `ioctl(TIOCGETA)`. This now fails with ENOTCAPABLE, and `os_isatty` maps any failure to
       "not a tty", so the device is typed as a character device as before;
     - `fcntl(F_GETFL)`, from `fd_fdstat_get` and `os_file_get_access_mode`;
     - `writev` and `lseek`, from `fd_write` and `fd_seek`.
   - Hence `EngineDevice = WRITE|FSTAT|SEEK|FCNTL(GETFL)` (see the deviations).
   - WAMR's random source on FreeBSD is `arc4random_buf` (`ssp_config.h`, `CONFIG_HAS_ARC4RANDOM_BUF`). It
     opens no descriptor.
   - `pthread_attr_get_np` → `cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, own tid)` is allowed in
     capability mode (`kern_cpuset.c:1766-1781`, `cpuset_capmode_allowed`; libthr `thr_attr.c:152`).
5. **`kern.arandom` and `getrandom` work in capability mode.**
   - `getrandom` (563) is CAPENABLED (`syscalls.master`).
   - `kern.arandom` is `CTLFLAG_CAPRD` (`kern_mib.c:195-197`), and `__sysctl` is CAPENABLED.
   - libc's `arc4random` stirs from `getentropy` → `getrandom`, falling back to `kern.arandom`
     (`getentropy.c:80-133`, `arc4random.c:136`), and maps its state with `mmap` and `minherit`, both
     CAPENABLED.
6. **`clock_gettime` is allowed.** `clock_gettime` (232) is CAPENABLED. libc reads it through the vDSO anyway.
7. **Every `#ifdef __FreeBSD__` path compiles against real headers.** I went further than the mind's eye.
   - I unpacked `usr/include` from FreeBSD **14.4-RELEASE** and **15.1-RELEASE** amd64 `base.txz` (both ship
     libc++ 19.1.7). (14.2 is no longer on download.freebsd.org.)
   - Each file was compiled with `clang++ --target=x86_64-unknown-freebsd14.4/15.1 --sysroot … -nostdinc++
     -isystem …/c++/v1 -std=c++23 -fexperimental-library -fsyntax-only` under the project's warning set and
     `-Werror`. The `-fexperimental-library` flag is what the top-level CMake adds on libc++, for `jthread`.
   - Files checked, with **0 diagnostics** on both versions:
     - `Sandbox_freebsd.cpp`, `HostBootstrap_posix.cpp`, `DescriptorRights.cpp`, `HostProcess.cpp`;
     - `SandboxProbe.cpp`, `LaunchProbe.cpp`, `SandboxProbe_test.cpp`, `ProcessLauncher_test.cpp`;
     - `Sandbox_test.cpp`, `HostProcess_test.cpp`, `WamrEngine.cpp`, `WamrEngine_test.cpp`.
   - The aarch64 target could not be checked, because the amd64 tarball carries amd64 `machine/` headers.
   - I avoided `std::ranges::contains` in FreeBSD-only code. libc++ 19 has it, but the CI report shows that
     standard libraries differ on it.

Other facts I relied on:
- **ioctl.** `kern_ioctl` checks only the descriptor's ioctl *list* (`sys_generic.c:755-759`,
  `cap_ioctl_check` at `sys_capability.c:356-380`; -1 means all). `kern_cap_rights_limit` empties that list when
  the new rights lack `CAP_IOCTL`, and zeroes the fcntl mask when they lack `CAP_FCNTL`
  (`sys_capability.c:248-254`). So "ioctls none" follows from leaving out `CAP_IOCTL`, and F_GETFL needs
  `CAP_FCNTL` in the rights before `cap_fcntls_limit(CAP_FCNTL_GETFL)` can narrow it.
- **fcntl.** `F_GETFD` and `F_SETFD` need no right; `F_GETFL` and `F_SETFL` need `cap_fcntl_rights`
  (`kern_descrip.c` `kern_fcntl` 528-561). `cap_fcntls_limit` rejects only bits outside `CAP_FCNTL_ALL`, or a
  widening (`sys_capability.c:586-600`).
- **RLIMIT_NPROC.** `chglimit()` enforces a limit only `if (diff > 0 && max != 0)` (`kern_resource.c`). A
  limit of 0 is "none", so **the old FreeBSD cap of 0 never refused a fork. That is a pre-existing bug, fixed
  here** with 1.
  - `fork1()` charges the real uid's count (`cr_ruidinfo`), which already counts this process, so 1 refuses
    every fork, with EAGAIN.
  - A caller holding `PRIV_PROC_LIMIT` (root) bypasses the limit (`kern_fork.c`, `fork1`).
- **Calls refused in capability mode.** `open` (5), `connect` (98), `execve` (59), `ptrace` (26) and `wait4`
  (7) are not CAPENABLED. `kill` is refused unless the target is the caller's own pid (`kern_sig.c:1910-1914`).
  `socket` (97) **is** CAPENABLED, and `kern_socket` has no capability-mode check (`uipc_syscalls.c:141-171`).
- **Man pages read.**
  - cap_rights_limit(2): rights only shrink; ENOTCAPABLE when widening.
  - cap_fcntls_limit(2): the `CAP_FCNTL_*` list.
  - procctl(2) 14.2: `PROC_NO_NEW_PRIVS_CTL`, `PROC_NO_NEW_PRIVS_ENABLE`, cannot be undone.
  - cap_enter(2): descendants are placed in capability mode; global namespaces are denied.
  - All at man.freebsd.org, manpath FreeBSD 14.2-RELEASE. `PROC_NO_NEW_PRIVS_CTL` is in `sys/procctl.h:64` of
    14.2 and 14.4.

## Step 3: RLIMIT_AS
It is set to `dataBytes`, as the brief says (Contour's default is 1 GiB; the probe tests use 64 MiB). WAMR,
built by CPM with `WAMR_DISABLE_HW_BOUND_CHECK 1`, reserves no 8 GiB guard, and a system WAMR built with
hardware bounds checks is refused at configure time. **The `procstat -v` measurement CI was to supply is
outstanding**, because CI has not run.

## Local results (HEAD after the commits, out/clang-asan)
- Build (`cmake --build --preset clang-asan -j 4`, niced, behind `flock -o`): every vtextension test target,
  `extension_host_probe` and the three `contour-extension-host[-wamr|-hostile]` executables built with
  0 warnings.
- `ctest -R 'vtextension|wasm_runtime_containment'`: **8/8 passed**. The suites run directly:

  | suite | result |
  |---|---|
  | `vtextension_process_test` | 135 cases: 134 passed, 1 skipped |
  | `vtextension_remote_test` | 348 cases: 347 passed, 1 skipped |
  | `vtextension_helper_test` | 161 cases: 160 passed, 1 skipped |
  | `vtextension_wamr_helper_test` | 62 cases, 1237 assertions, all passed |
  | `vtextension_test` | 484 cases, 26485 assertions, all passed |

  The skips predate this change: the ones in the remote, helper and process suites are cases that only run
  where there is no sandbox, or on another system.
- `vtextension_process_test "[sandbox]"`: 21 cases and 4566 assertions passed, including the Linux
  `connect-inet → EPERM` row.
- `ctest -L lint`: **33/33 passed**.
- `CLANG_TIDY=clang-tidy ./scripts/clang-tidy-diff.sh --base origin/master --build-dir out/clang-asan`, with
  JOBS=4, run over the whole branch diff:
  - It found 2 findings, both `performance-unnecessary-value-param` on test lambdas that took
    `SandboxDescriptors` by value (`HostProcess_test.cpp:727`, `HostileHost.cpp:107`). Both are fixed by
    taking a `const&`.
  - A rerun of clang-tidy on those two TUs is clean.
  - `Sandbox_freebsd.cpp` is not in the Linux compile database, so clang-tidy never sees it. Its only check is
    the FreeBSD-header syntax check above.
- `/usr/bin/clang-format --dry-run --Werror` (22.1.8) on every C++ file changed: clean.
- `editorconfig-checker` on every changed file: clean.
- `scripts/check-spelling.py` (typos 1.48.0): OK.
- The FreeBSD syntax check (Step 4, item 7): 12 files, 0 diagnostics, on both 14.4 and 15.1 amd64.
- Performance: none on any hot path. The host builds one vector of about eight entries once at start-up.
- Coverage: not measured (no clang-coverage build, per run rules).

## What remains unproven
- Nothing has run on FreeBSD:
  - capability mode with the limited descriptors;
  - the FreeBSD probe expectations;
  - the WAMR helper locked down under Capsicum (`vtextension_wamr_helper_test`);
  - whether 64 MiB of address space (`HostBootstrap.capsHold`) and 1 GiB (the helpers) suffice for a FreeBSD
    process. Measuring this with `procstat -v` is Step 3, and it is outstanding.
- `send-descriptor`'s outcome on FreeBSD is logged, not asserted.
- `fork` and `pdfork` count as allowed for root. A run as root (the CI VM is probably one) therefore proves
  only that they do not kill the probe; EAGAIN is proven only for an unprivileged user.
- The descriptors the host holds outside `SandboxDescriptors` keep every right they have:
  - stdin and stdout (the null device);
  - the wake receiver's own stop pipe.
  None of them reaches anything global, and capability mode still applies.

## FreeBSD CI trigger: recommendation
- The `freebsd` job has `if: github.event_name != 'pull_request'`.
- After this lands, the **sandbox is live in FreeBSD's tests**: `VTEXTENSION_FREEBSD_SANDBOX` is on, the
  helper and probe lock down, and only the *default* host stays in-process.
- So its first real run is a merge-queue or master push. A mistake would turn the master FreeBSD leg red.
- I recommend that the owner run the `freebsd` job once on this branch before merging. Either:
  - add `workflow_dispatch` (or a label-gated `pull_request` condition) to that job; or
  - push the branch to a scratch branch that triggers it.
- Then add the `FreeBSD:...` row to `_CONTOUR_SANDBOXED_PLATFORMS` in a follow-up once it is green (D110).
- I did not change the triggers.

## Deviations
1. **FreeBSD does not join `_CONTOUR_SANDBOXED_PLATFORMS`.** This follows the controller ruling, against the
   brief's file list. The sandbox is still built and used by the extension host on FreeBSD, through the new
   `VTEXTENSION_FREEBSD_SANDBOX`. Otherwise its tests could never run there, and the code would not compile in
   CI at all.
2. **The EngineDevice rights are `WRITE|FSTAT|SEEK|FCNTL(GETFL)`, not `WRITE|FSTAT`.** WAMR calls `lseek`
   (`fd_seek`) and `fcntl(F_GETFL)` (`fd_fdstat_get`, and at instantiation) on the device, and the backend
   contract (`EveryWasiFunctionOutcomes`) asserts that both succeed. With only `WRITE|FSTAT` both would fail
   ENOTCAPABLE, and `vtextension_wamr_helper_test` would fail on FreeBSD.
3. **GetStatusFlags (`CAP_FCNTL` limited to `F_GETFL`) is on every role.** I read "fcntls F_GETFL" as a rule
   for the whole table. `CAP_FCNTL` is needed for `cap_fcntls_limit` to mean anything, and F_GETFL is
   read-only.
4. **`socket` → allowed on FreeBSD, not ECAPMODE.** The FreeBSD source shows `socket(2)` is CAPENABLED with no
   check in capability mode. What capability mode refuses is the address, so a `connect-inet` op was added
   (FreeBSD: ECAPMODE; Linux: EPERM, from socket).
5. **`fork`, not only `pdfork`.** Both are expected to fail EAGAIN, with the root and sanitizer exemptions
   above.
6. **RLIMIT_NPROC on FreeBSD is 1, not 0.** 0 means "no limit" there (see above). This is a pre-existing bug,
   fixed here.
7. **`IHostedEngine::heldDevices()` is a new pure virtual.** It was needed to give the null device its role.
   All four implementers were updated.
8. **The probe applies `capResources` when sandboxed, on Linux as well.** That is what the host does. In
   sanitizer builds it is a no-op (CapPolicy::Skip), so on Linux it is exercised only by CI's non-sanitizer
   builds.

## Concerns
- The first FreeBSD run will happen after merge (see the recommendation). The most likely failures:
  - an address-space cap too small for the probe (64 MiB);
  - an unexpected `ENOTCAPABLE` from a libc path I did not read;
  - root changing an expectation.
- On Linux the sandboxed probe now caps itself as well (`RLIMIT_NPROC 0`, `RLIMIT_DATA 1 GiB`). I checked by
  reading that seccomp kills fork and clone before the rlimit is consulted. This path was not run locally,
  because ASan skips the caps.

## Fix round 1

The commits were rewritten. Each fix was folded into the commit it belongs to (fixup, then `--autosquash`
onto `origin/master` 8c3afaa0c after `git fetch`), and both messages were reworded to match:
- `9e44f07fa` refactor(vtextension): the extension host names every descriptor it keeps, and what for
- `5a5ed1500` feat(vtextension): the extension host enters capability mode on FreeBSD

I checked out the refactor commit on its own (as `3515f78a1`, before the two tidy fixes below were folded in) and built it: every vtextension test target, the probe and the three
helpers built with 0 warnings. Its focused suites passed: `HostProcess.*`, `Sandbox.*`, `WakeSignal.*`,
`[sandbox]` and `WamrEngine.*`.

### 1. RLIMIT_AS (Important)
- **What a helper maps, measured on Linux.** I measured `out/clang-release/.../contour-extension-host-wamr`,
  a release build, which was already there. While the release `vtextension_wamr_helper_test` ran, I sampled
  each helper child through `/proc/<test>/task/*/children`:
  - There were 22 helpers. Their VmSize at the first sample was 302 to 337 MiB (median 313 MiB); the peak
    VmPeak was 440 MiB and VmRSS 18 MiB.
  - The mappings could be read only before `PR_SET_DUMPABLE 0`. At that point there were 15.6 MiB of program
    text and libraries.
  - The stacks are computed from the constants, not measured: 4 executors of `ExecutorStackBytes` (8 × 1.25
    MiB, so 10 MiB each), plus the listener's 256 KiB, make 40.25 MiB.
  - The remaining ~250 MiB is, by inference, glibc's per-thread malloc arenas: one 64 MiB reservation per
    thread, which is mostly `PROT_NONE`. FreeBSD's jemalloc makes no such reservation.
- **The cap.** `AddressSpaceHeadroomBytes = 256 MiB` (HostBootstrap.hpp, with the reasoning above at the
  declaration). The cap is `addressSpaceCap(d) = 2·d + 256 MiB`:
  - It saturates rather than wrapping, and `asLimit()` clamps it to FreeBSD's signed `RLIM_INFINITY`.
  - Text, libraries and stacks come to about 57 MiB before the heap, so the headroom leaves several times
    that for the allocator.
  - The arithmetic and the mremap reasoning are in the decision comment at `Caps`, in HostBootstrap_posix.cpp.
  - With Contour's default `DefaultDataBytes` of 1 GiB, the cap is 2.25 GiB; the probe's 64 MiB gives 384 MiB.
- `HostBootstrap.capsHold` (FreeBSD) expects `as=addressSpaceCap(ProbeDataBytes)`.
- **CMake.** `src/vtextension/CMakeLists.txt` fails with FATAL_ERROR when the FreeBSD sandbox meets
  `CONTOUR_WASM_RUNTIME=wasmtime`. The engine is resolved before `add_subdirectory(src)` (CMakeLists.txt:472
  vs :495). FreeBSD's CI installs no Wasmtime (`install_deps_FreeBSD`), so it resolves to wamr.
  - Note: if a Wasmtime were *found* on FreeBSD while the engine is wamr, the Wasmtime-based test helpers
    (`vtextension_helper_test`) would still be built and would run under the cap. The ruling named the
    engine, so that case is not refused.

### 2. The rights of every held descriptor are now tested (Important)
- **New neutral API: `DescriptorLimit { Unlimited, AsItsRole, Otherwise }`, read by `limitOf(HeldDescriptor)`.**
  It is defined per system.
  - On Linux, and where there is no sandbox, it is `Unlimited`.
  - On FreeBSD it reads `cap_rights_get` and compares both ways (`cap_rights_contains`) with the rights the
    sandbox builds from `rightsOf(role)`. The same `capabilitiesOf()` and `fcntlsOf()` serve both the
    limiting and the check. It also requires that `cap_fcntls_get` returns that role's fcntls (`CAP_FCNTL_GETFL`
    or 0) and that `cap_ioctls_get(fd, nullptr, 0) == 0`.
  - A descriptor holding `CAP_ALL` is `Unlimited`.
- **New probe op `held-limits`.** It reports one word per held descriptor (lanes, then standard input, output
  and error).
- **New test `Sandbox.heldDescriptorsAreLimitedToTheirRoles`.** It runs on both systems: FreeBSD expects
  `as-its-role` × 5, Linux expects `unlimited` × 5. It passes on Linux.
- **`no-new-privs=` line.** It is read after `restrictBeforeThreads()` and before the lockdown, because
  `procctl` is not CAPENABLED:
  - FreeBSD uses `PROC_NO_NEW_PRIVS_STATUS`, under its `#ifdef`;
  - Linux uses `PR_GET_NO_NEW_PRIVS`.
  The test expects `on` where the system can say, and `unknown` otherwise.

### 3. Descriptors that kept every right (Minor)
- **New roles:**
  - `IdleStream` (`Fstat` only) for fd 0 and fd 1;
  - `StopReceiver` (`Read|Event`) and `StopSender` (`Write`) for the wake receiver's stop pipe.
- **Where the handles come from:**
  - `WakeReceiver::stopHandles()` (new) returns the stop pipe's two ends. On Windows both name the one event.
  - `standardStreams()` replaces `standardErrorHandle()` and returns all three streams with their roles.
- **Who uses them:**
  - The host hands the sandbox all of them.
  - The probe hands its streams.
  - The host test checks the order, the roles and the stop pipe's distinct ends.
  - `WakeSignal.stopHandlesAreTheReceiversOwn` tests the new accessor.
- The Linux behaviour is unchanged: roles are labels there.

### 4. The probe's caps (Minor)
- `capResources`' result now goes into `lockdown=`, which names the first of restrict, cap and lockdown that
  failed, as the host does.
- Every Linux sandboxed test checks `lockdown == "locked"`. Where CI's non-sanitizer Linux builds apply the
  caps, that line proves they were set. In ASan they are skipped.

### 5. scripts/check-no-threaded-fork.py (Minor)
The reason now says that the `fork`/`pdfork` ops prove the sandbox stops a fork: Linux's filter kills it before
any child exists, and FreeBSD's process cap refuses it. Where a fork does succeed (unsandboxed, or as root on
FreeBSD), the child only calls `_exit()`. The script passes.

### 6. MSVC /W4 (Minor)
`DescriptorRights.cpp`'s `static_assert` (and the same one in `Sandbox_freebsd.cpp`) now compares
`std::size_t { std::to_underlying(...) } == index`.

### Results after the fix round (HEAD `5a5ed1500`)
- Build: 0 warnings.
- `ctest -R 'vtextension|wasm_runtime_containment'`: 8/8 passed. The suites run directly:

  | suite | result |
  |---|---|
  | `vtextension_process_test` | 137 cases: 136 passed, 1 skipped |
  | `vtextension_remote_test` | 348 cases: 347 passed, 1 skipped |
  | `vtextension_helper_test` | 161 cases: 160 passed, 1 skipped |
  | `vtextension_wamr_helper_test` | 62 cases, 1237 assertions, all passed |
  | `vtextension_test` | 484 cases, 26485 assertions, all passed |

  `[sandbox]` alone: 22 cases, 4583 assertions.
- `ctest -L lint`: 33/33 passed.
- clang-format dry run, editorconfig-checker, typos and check-no-threaded-fork: clean.
- tidy-diff (`CLANG_TIDY=clang-tidy ./scripts/clang-tidy-diff.sh --base origin/master --build-dir
  out/clang-asan`, JOBS=4):
  - The first run found 2 issues: a nested conditional in the probe's `noNewPrivileges()`, and the new
    `WakeSignal` test, which had landed inside an existing `#ifndef _WIN32` block (a redundant nested
    `#ifndef`).
  - Both were fixed: the test now sits outside that block and runs on Windows too. Each fix was folded into
    its commit.
  - A full rerun reports **147 translation units clean**.
- Windows cross build (`out/clangcl-debug`: `vtextension_process_test`, `extension_host_probe`): built. Under
  wine (own prefix), `WakeSignal.*` passed: 5 cases, 23 assertions.
- The FreeBSD syntax check, against the 14.4 and 15.1 amd64 headers: 14 files, 0 diagnostics on both. The
  files are the 12 from before plus `WakeSignal_posix.cpp` and `WakeSignal_test.cpp`.

### Still unproven
- Everything listed under "What remains unproven" above.
- Whether jemalloc on FreeBSD stays well inside the 256 MiB headroom. The Linux measurement covers text and
  stacks only. Its malloc arenas are not comparable to jemalloc's.
