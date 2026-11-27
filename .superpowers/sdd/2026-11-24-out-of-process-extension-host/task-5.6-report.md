# Task 5.6 report: the macOS sandbox and the helper's entitlement

Status: DONE_WITH_CONCERNS. Everything that can be checked on Linux passes. The macOS behaviour can only be
proven by macOS CI (see "What the first macOS CI run must show"). Not pushed.

BASE `71cbaa680`, HEAD `71d789007`.

## Commits

| Commit | Subject | Content |
|---|---|---|
| `c0374cb3a` | build: pick wamr by default where the extension host's sandbox cannot run wasmtime | Item 0 (both points) |
| `07694d342` | feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS | profile, `Sandbox_macos.cpp`, CMake wiring, probe port, tests |
| `71d789007` | build: sign the macOS extension host with the one entitlement its compiler needs | entitlements file, `macos-bundle.py` + `--self-test` gate, docs, `build.yml` |

### Item 0 (`c0374cb3a`)
- `CMakeLists.txt`: a new table, `_CONTOUR_SANDBOXES_WITHOUT_WASMTIME` (rows are `CMAKE_SYSTEM_NAME`, with
  `FreeBSD` as the only row), carries the reason as a comment. The automatic choice skips wasmtime on
  systems the table lists. An explicit `CONTOUR_WASM_RUNTIME=wasmtime` on such a system is refused, by the
  same row and next to the other explicit-engine check. The FreeBSD-only `FATAL_ERROR` in
  `src/vtextension/CMakeLists.txt` is replaced by a pointer to that row, so the rule exists once.
- `HostBootstrap.cpp`: adds
  `static_assert(FixedMappingBytes * 4 <= AddressSpaceHeadroomBytes)`.
  `FixedMappingBytes = ProgramImageBytes (16 MiB) + MaxLaneChannels × ExecutorStackBytes +
  InterruptListenerStackBytes`, which is 56.25 MiB today against a 64 MiB bound. So the heap keeps at least
  three times what the program and its stacks take, which is the "several times that" the prose promised.
  The prose in `HostBootstrap.hpp` now points at the assert. Doubling the lanes (to 8) would come to about
  96 MiB × 4 > 256 MiB, which fails to compile.

### Sandbox (`07694d342`)
- `src/vtextension/wasm/host/macos/SandboxProfile.hpp` (std-only): holds `SandboxProfile` (constexpr SBPL
  text), the `ProfileParameters` rows `{name, value}` (`SHM_PREFIX` → `SharedObjectNamePrefix`), and
  `profileParameterList()` (name, value, …, nullptr).
- `src/vtextension/wasm/host/Sandbox_macos.cpp`: `MacSandbox`. `restrictBeforeThreads()` does nothing.
  `lockDown()` calls `sandbox_init_with_parameters`; if that fails, the result is
  `ConfinementFailure{Profile, errno or EINVAL}`, and `errorbuf` is written to stderr as
  `contour-extension-host: sandbox profile: …` and then freed. `limitOf()` returns `Unlimited`. The file
  comment restates that Wasmtime's mach ports are off and that macOS has no parent-death binding.
- `SharedObjectName.hpp`: adds `SharedObjectNamePrefix = "/contour-eh-"`, a C string, and
  `sharedObjectName()` now uses it.
- CMake: `VTEXTENSION_MACOS_SANDBOX` is ON for Darwin (as FreeBSD's is: built, tested and run, but **not**
  a row of `_CONTOUR_SANDBOXED_PLATFORMS`, per D110). `vtextension_host` selects `Sandbox_macos.cpp` with
  `PlatformSandbox` 3. The probe's sandboxed mode and `SandboxProbe_test.cpp` now build there too (in
  `LaunchProbe.cpp`, `CONTOUR_PLATFORM_SANDBOX == 3`).
- `SandboxProbe.cpp` port:
  - `/etc/hosts` instead of `/etc/hostname` on macOS;
  - `SOCK_CLOEXEC` and `MSG_NOSIGNAL` only where they exist;
  - `PT_TRACE_ME` on macOS;
  - `::mprotect` and `::clock_gettime` instead of `syscall()` outside Linux, because `syscall()` is
    deprecated on macOS and would break `-Werror`;
  - `getentropy()` on macOS;
  - a new macOS-only op, `mach-lookup` (`bootstrap_look_up` of `com.apple.system.logger`).
- Tests:
  - `Sandbox.profileHasNoNetworkOrMachLookup` (runs on every system): the profile starts with
    `(version 1)\n(deny default)`. It contains none of `(allow default`, `network`, `mach-lookup`,
    `file-read`, `file-write`, `process-fork`, `process-exec`, `ipc-posix-shm-write-create`, `-unlink` or
    `-truncate`. It grants shm by `(ipc-posix-name-prefix (param "SHM_PREFIX"))`.
  - `Sandbox.profileParametersAreWhatTheProfileReads` (extra): every parameter is read by the profile, and
    the prefix is what `sharedObjectName()` produces. It also checks the C list's shape.
  - The macOS rows in the probe's `ForbiddenOperations`: `open-file`, `open-directory`, `socket-inet`,
    `socket-unix`, `connect-inet`, `fork`, `execve` and `signal-parent` give `EPERM`; `mach-lookup` gives
    `BOOTSTRAP_NOT_PRIVILEGED` (1100).
  - `held-limits` expects `unlimited`, `no-new-privs` expects `unknown`, and `permitted` (shm mapped after
    lockdown) runs unchanged on macOS.

### Entitlement (`71d789007`)
- `support/macOS/contour-extension-host.entitlements`: holds only
  `com.apple.security.cs.allow-unsigned-executable-memory`, with no XML comment (AMFI rejects them).
- `scripts/macos-bundle.py`:
  - `NESTED_ENTITLEMENTS` is a table of bundle-relative path → entitlements file, with one row:
    `Contents/Helpers/contour-extension-host`.
  - The pure `signing_commands()` is split out of `sign()`, and `entitlements_of()` gives the bundle the
    app's file, a listed row its own file, and everything else none.
  - `sign()` checks that every file exists.
  - The order is unchanged: nested binaries, including the helper, are signed before the executable and
    the bundle.
- `--self-test`:
  - It builds a stand-in bundle (Mach-O magic, Info.plist) and asserts six things: every item is signed;
    every signature carries `--options=runtime`; only the helper and the bundle get `--entitlements`, each
    its own; the helper is signed before the bundle; the bundle is signed last; and the helper file holds
    exactly the one key with no comment.
  - It is registered as the lint ctest `check_macos_bundle_selftest`.
  - A mutation run (giving the main executable an entitlement) made it fail, as it should.
- `docs/internals/macos-code-signing.md`: documents the helper's entitlement and why Contour itself never
  gets it.
- `.github/workflows/build.yml`, in `macOSArm` (it runs on PRs):
  - The step "Extension host: compiling, signed as it ships, in its sandbox" runs whenever the Build step
    succeeded, even after a failed Tests step. It reconfigures `macos-ci` with
    `-DCONTOUR_EXTENSION_HOST=out-of-process`, which picks wasmtime automatically, and builds
    `vtextension_helper_test`.
  - It signs the helper with
    `codesign --force --sign - --options=runtime --entitlements support/macOS/contour-extension-host.entitlements`,
    prints the embedded entitlements, and runs `vtextension_helper_test "~[.measure]"`. `HelperLockdown` is
    `RequireLockedDown` now that macOS has a `PlatformSandbox`.
  - It then re-signs the helper without the entitlement and requires
    `vtextension_helper_test "Process.loadsAndCallsThroughARealHelper"` to **fail**.
  - The step "Sandbox deny log" runs on `failure()` and prints
    `log show --last 90m --style compact --predicate 'sender == "Sandbox"' | grep deny | tail -n 400`.

## Step 5 checklist

| Item | Verdict | Evidence |
|---|---|---|
| Declaration matches Chromium's | Yes | Chromium `sandbox/mac/seatbelt.cc` (main, fetched 2026-11-26) has `int sandbox_init_with_parameters(const char* profile, uint64_t flags, const char* const parameters[], char** errorbuf);`, and `sandbox_free_error(char*)` comes from `<sandbox.h>`. Ours has the same types, in an `extern "C"` block, with Mach-O asm labels (`_sandbox_init_with_parameters`, `_sandbox_free_error`) so that the C++ names stay camelBack. `SanitizerOptions.cpp` uses the same idiom in this repo. The parameters are null-terminated name/value pairs, like Chromium's `InitWithParams` ("The parameters array must be null terminated"). Flags 0 means profile text, as Chromium passes. |
| `-Werror` sees no deprecated symbol | Yes, by construction; CI must confirm | `<sandbox.h>` is not included anywhere. Chromium needs `#pragma clang diagnostic ignored "-Wdeprecated-declarations"` around `sandbox_free_error` because it includes that header; our own declaration has no attribute. The probe no longer calls `syscall()` (deprecated on macOS) outside `__linux__`. `bootstrap_look_up`/`mach_port_deallocate` are not deprecated. `Sandbox_macos.cpp` was syntax-checked on Linux with the tree's full `-Werror -Wall -Wextra …` flags (it is std-only apart from the declarations): clean. |
| shm grants are read/write-data only, never create (R38) | Yes, plus read-metadata (see Deviations) | The rule is `(allow ipc-posix-shm-read-data ipc-posix-shm-read-metadata ipc-posix-shm-write-data (ipc-posix-name-prefix (param "SHM_PREFIX")))`. There is no `write-create`, `write-unlink` or `write-truncate`, and the profile test asserts their absence. The host creates no object: `createSharedObject` is called only by Contour (grep: only `HostServer_test.cpp` within `host/`). |
| `MAP_SHARED|MAP_FIXED` of a received shm descriptor needs no `file-map` rule | Yes | XNU `bsd/kern/posix_shm.c`: mmap of a POSIX shm descriptor goes to `pshm_mmap`, which calls `mac_posixshm_check_mmap(cred, &pinfo->pshm_hdr, prot, flags)`, the shm hook (`ipc-posix-shm-*`), not the vnode path that `file-map-executable` mediates. We map RW, never exec. The object keeps its name and label after `shm_unlink` until the last reference, so the prefix filter still matches the unlinked objects Contour hands over. The `permitted` probe op (map after lockdown) proves it on macOS CI. |
| Phase 4 open macOS item: `CMSG_SPACE` as an array bound | Yes by reading; CI compiles it | XNU `bsd/sys/socket.h:717`: `#define CMSG_SPACE(l) (__DARWIN_ALIGN32(sizeof(struct cmsghdr)) + __DARWIN_ALIGN32(l))`, and `bsd/arm/_param.h:21`: `__DARWIN_ALIGN32(p) ((__darwin_size_t)((__darwin_size_t)(p) + __DARWIN_ALIGNBYTES32) &~ __DARWIN_ALIGNBYTES32)`, with `__DARWIN_ALIGNBYTES32 = sizeof(__uint32_t) - 1`. That is casts and arithmetic on constants, so it is an integral constant expression and a valid `std::array` bound (`Channel_posix.cpp:119,122`, `SandboxProbe.cpp` sendDescriptor, `PduStream_test.cpp:329`). |
| Phase 4 open macOS item: BSD descriptor attribution | Outstanding for CI | `PduStream::attachedFrameStart()` (`PduStream.cpp:236-254`): outside Linux the handle goes with the first frame that starts in what arrived (BSD semantics: a receive ends before a message carrying rights). Nothing here changes it. `descriptorGoesWithItsFrame` and `instantiatesConcurrently` (in `vtextension_remote_test`/`process_test`) run in macOS CI's ctest, and the `permitted` probe case and the whole helper suite now exercise descriptor receipt *under the sandbox* on macOS. |

## Local results (Linux, `out/clang-asan`)

- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment|macos_bundle'`: **9/9 passed**
  (`vtextension_test`, `_wasmtime_test`, `_process_test`, `_remote_test`, `_helper_test`,
  `_wamr_helper_test`, `check_wasm_runtime_containment`, its selftest, and `check_macos_bundle_selftest`).
- `vtextension_remote_test "Sandbox.*"`: 6 cases, 5 passed and 1 skipped (the skip is
  `systemWithoutOneRunsUnsandboxed`, which skips on a system that has a sandbox); 77 assertions.
- `vtextension_process_test "[sandbox]"`: 22 cases, 4583 assertions passed. I re-ran it after the final
  probe edit, which changed only comments.
- `CHECK_WORKFLOWS_REQUIRE_TOOL=1 CHECK_SPELLING_REQUIRE_TOOL=1 ctest --preset=clang-asan -L lint`:
  **34/34 passed**. This includes `check_workflows`, which runs actionlint on `build.yml`.
- `python3 -I scripts/macos-bundle.py --self-test`: `7 cases OK` (the six assertions above, with the
  entitlements file checked twice: its key and its lack of comments). The mutation run failed, as it
  should.
- CI's tidy-diff (`JOBS=4 scripts/clang-tidy-diff.sh --base 71cbaa680 --build-dir out/clang-asan`, local
  clang-tidy 22): **6 translation units clean**. `Sandbox_macos.cpp` is not in the Linux compile database
  ("another platform's file"), so I ran clang-tidy on it directly with an interpolated command. That run
  found three findings, now fixed: string_view `data()`, an unnecessary value parameter, and
  simplify-subscript. It is now clean.
- `/usr/bin/clang-format --dry-run --Werror` (22.1.8) on every changed C++ file: clean.
- `editorconfig-checker` on the changed files: clean.
- `scripts/check-spelling.py` (typos 1.48.0): OK.
- Per-commit builds:
  - `c0374cb3a`: `vtextension_host` built at that state.
  - `07694d342` and `71d789007`: the full vtextension set (all six test binaries, `contour-extension-host`,
    the probe) built with the working tree equal to `07694d342`'s C++ plus `71d789007`'s non-C++ files.
    `71d789007` changes no C++.
- No macOS build or run is possible here.

## What the first macOS CI run must show (`macOSArm`)

1. **Compile**: the whole tree builds with Apple clang under `-Werror`. If it fails, the likely places are
   `Sandbox_macos.cpp` (asm labels, `uint64_t` in the declaration), `SandboxProbe.cpp`'s `__APPLE__`
   branches (`<servers/bootstrap.h>`, `getentropy`, `PT_TRACE_ME`), and the `SandboxProbe_test.cpp` macOS
   table.
2. **Tests** (the plain ctest): `vtextension_process_test` must pass its `[sandbox]` cases on macOS:
   - `forbiddenOperationsFailFromAnExecutor`: each macOS row refused as expected;
   - `heldDescriptorsAreLimitedToTheirRoles`: `unlimited` ×5, and `no-new-privs=unknown`;
   - `permittedOperationsWorkFromAnExecutor`: shm map after lockdown gives `ok`.

   `vtextension_wamr_helper_test` must pass with every helper held to `RequireLockedDown`: the WAMR helper,
   unsigned and not hardened, running inside the profile. `vtextension_remote_test` must pass
   `Sandbox.profile*`.
3. **The extension-host step**:
   - The positive run, `vtextension_helper_test "~[.measure]"`, must be all green: Wasmtime compiling inside
     the profile under the hardened runtime with the entitlement.
   - The `codesign -d --entitlements` print must show the one key.
   - The negative run must fail, and the step must not print the `::error::`.
4. **If anything fails**, the "Sandbox deny log" step prints lines like
   `Sandbox: contour-extensio(1234) deny(1) sysctl-read hw.foo` or
   `… deny(1) ipc-posix-shm-read-metadata /contour-eh-…`. Read it this way:
   - The kernel shortens process names to 16 characters, so look for `contour-extensio` (the helper) and
     `extension_host_p` (the probe). Ignore other processes.
   - `sysctl-read <name>` → add `(sysctl-name "<name>")` to the `sysctl-read` rule in
     `SandboxProfile.hpp`, with the reason (which library reads it).
   - An `ipc-posix-shm-*` op on `/contour-eh-…` → add only that read/write op. Never `write-create`, and
     the profile test enforces that.
   - `mach-lookup`, `file-*`, `network*` or `process-*` from the helper → **do not allow it**. Find the
     caller after lockdown and move the call before `lockDown()` or remove it. The profile test forbids
     those words.
   - A probe op whose errno differs from the table (for example `mach-lookup` refused with 1102 rather
     than 1100, or `signal-parent`/`execve` not `EPERM`) → correct the expectation row. That is the
     system's word, not a sandbox gap.
   - A failure with no deny line at all (for example a helper killed by `SIGKILL` with "CODESIGNING" in the
     crash log) points at the entitlement or the hardened runtime, not at the profile.

## Deviations

1. **`ipc-posix-shm-read-metadata` is granted beside read-data/write-data.** The host `fstat()`s every
   granted object (`checkSharedObjectSize`, called from `SharedMapping::map`). XNU's `pshm_stat` calls
   `mac_posixshm_check_stat`, which Seatbelt mediates as shm metadata. Without the grant, the first map
   after lockdown would very likely fail. It is still never create, and the WebKit `WebProcess.sb` grants
   the same family as `ipc-posix-shm-read*` (both data and metadata). If CI shows it is not needed, drop it
   in one line.
2. **sysctl allow-list**: only `(sysctl-name-prefix "hw.optional.")`. It is the CPU feature probe that
   Cranelift's native ISA detection reads, through Rust's `std_detect` on Apple aarch64 (`sysctlbyname
   hw.optional.arm.FEAT_*`). It runs once, on the first module compiled, which happens after lockdown.
   Both WebKit (`WebProcess.sb`, `rdar://71462790`) and Chromium (`common.sb`) allow exactly this prefix. I
   added no other names. Parallel compilation is off, so `hw.ncpu`/`hw.activecpu` are not read after
   lockdown as far as I can tell. The deny log decides.
3. **`SharedObjectNamePrefix` is a `char const*`, not a `string_view`**: the profile's parameter rows must
   hand NUL-terminated strings to the C API, and clang-tidy (bugprone-suspicious-stringview-data-usage)
   rejects `string_view::data()` for that.
4. **`MacSandbox` keeps the `SandboxDescriptors` it is handed, unused.** clang-tidy's
   `performance-unnecessary-value-param` rejects every way of discarding the by-value parameter that the
   shared signature prescribes. FreeBSD keeps them the same way. The member type has a non-trivial
   destructor, so `-Wunused-private-field` does not fire.
5. **Probe expectations beyond the four the brief names**: `open-directory`, `connect-inet`, `execve` and
   `signal-parent` (all `EPERM`) were added, because they are what the profile claims to refuse. If macOS
   reports a different errno, adjust the row (the deny log shows the op either way).
6. **CI case**: the helper step reconfigures the same `macos-ci` tree out of process, rather than building
   a second tree, because a second tree would redo the vcpkg install. It runs even when the Tests step
   failed. The negative case is a single test name (`Process.loadsAndCallsThroughARealHelper`); the full
   suite passes with the entitlement first, so the signature is the only difference.
7. The brief's "Files" list says `CMakeLists.txt (Darwin appended)`. The controller's note overrides it:
   Darwin is **not** appended to `_CONTOUR_SANDBOXED_PLATFORMS`. The top-level CMakeLists change in this
   task is only the lint gate (plus Item 0).
8. `Helper.locksDown` stays Linux-only (`#ifdef __linux__`). `Process.unsandboxedIsRefusedUnlessAccepted`
   already asserts a locked-down helper on macOS through `ThisBuildLockdown`.

## Concerns

- **Nothing here has run on macOS.** Expect one or two iterations. The most likely ones, by my estimate:
  - a sysctl or `read-metadata` decision;
  - the exact errno or `kern_return_t` of `execve`, `signal-parent` and `mach-lookup` under Seatbelt;
  - an Apple-clang-only diagnostic in the probe's `__APPLE__` branches.
- **The `(version 1)` profile has no `dynamic-code-generation` rule.** WebKit allows that op only in its
  `SANDBOX_VERSION_3` profiles, and Chromium's v1 renderer runs V8 without one, so I expect v1 not to gate
  it. If the positive helper run fails with a deny of it, that is the one op that may need allowing, and it
  would contradict nothing in R38.
- The main ctest now runs `vtextension_wamr_helper_test` under `RequireLockedDown` on macOS, so a profile
  problem turns the required `macOSArm` check red. That is intended (prove before merge), but it means the
  PR is red until the profile is right.
- `log show` on GitHub's macOS runners may redact or rate-limit sandbox reports. If the deny log comes back
  empty on a failure, the fallback is to add `(debug deny)` temporarily, or to run the probe with
  `sandbox-exec`, in a follow-up CI tweak.

## Sources

- Chromium `sandbox/mac/seatbelt.cc` (raw.githubusercontent.com/chromium/chromium/main): the declaration,
  the deprecation pragmas, the null-terminated parameters.
- Chromium `sandbox/policy/mac/common.sb`: `(deny default)`, `(allow signal (target self))`, the
  `sysctl-read` list with `(sysctl-name-prefix "hw.optional.")`. Chromium `sandbox/policy/mac/renderer.sb`:
  `(allow ipc-posix-shm-read-data (ipc-posix-name …))`.
- WebKit `Source/WebKit/WebProcess/com.apple.WebProcess.sb.in` (main): `(allow ipc-posix-shm-read*
  ipc-posix-shm-write-data (ipc-posix-name-prefix "AudioIO"))`, `(sysctl-name-prefix "hw.optional.")`, and
  `dynamic-code-generation` only under `SANDBOX_VERSION_3`.
- XNU `bsd/kern/posix_shm.c` (apple-oss-distributions/xnu main): `pshm_mmap` →
  `mac_posixshm_check_mmap`, `pshm_stat` → `mac_posixshm_check_stat`, `shm_open` O_CREAT →
  `mac_posixshm_check_create`, and the name and label kept after unlink. XNU `bsd/sys/socket.h:717` and
  `bsd/arm/_param.h:21` for `CMSG_SPACE`.

## Fix round 1

Rebased with autosquash onto `origin/master` (`8c3afaa0c`, unchanged). HEAD is `4d2cadd1c`. Not pushed.

| Old | New | Subject |
|---|---|---|
| `c0374cb3a` | `c0374cb3a` (unchanged) | build: pick wamr by default where the extension host's sandbox cannot run wasmtime |
| `07694d342` | `bdc778691` | feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS (items 2, 4, 5) |
| `71d789007` | `4d2cadd1c` | build: sign the macOS extension host with the one entitlement its compiler needs (items 1, 3, 6, 7; message updated to match) |

The workflow's `on:` trigger lines are unchanged: `git diff 71cbaa680 -- build.yml` touches only the
`macOSArm` steps.

### 1. Negative entitlement case (Important)

**Deviation: "require the absence of SIGKILL" cannot hold on macOS.** On macOS the refusal *is* a
SIGKILL. Two sources show it:
- Wasmtime 48.0.10 (`crates/wasmtime/src/runtime/vm/sys/unix/mmap.rs`, `code_memory.rs:456`) maps code RW,
  then `mprotect`s it RX, without `MAP_JIT`. That is why the entitlement is
  allow-unsigned-executable-memory and not allow-jit.
- XNU `vm_map_protect` (`osfmk/vm/vm_map.c` ~5995-6035) refuses only W|X at once, so RW→RX succeeds and
  Wasmtime's `"unable to make memory executable"` context never fires. The refusal comes when the code
  first runs: `vm_fault` → `cs_invalid_page()` (`bsd/kern/kern_cs.c:274-309`, "CS_KILL triggers a kill
  signal") → `threadsignal(..., SIGKILL, EXC_BAD_ACCESS)`. Before that, `vm_fault.c:3011` prints
  `CODE SIGNING: process <pid>[<name>]: rejecting invalid page at address … from offset … in file "<path>"`,
  and `<path>` is `"<nil>"` for anonymous memory.

In the helper this happens in `prepareThread()`'s priming run, before Hello. Nothing of ours can print
first, so no stable text of our own can exist for it. The kernel's line is the stable text, and it tells
the two kills apart:
- for compiled code (the entitlement) it says `in file "<nil>"`;
- for a stale signature it names the helper's file.

The step now:
- signs a fresh copy and swaps it in (`cp` → `codesign` → `mv -f`), for both the positive and the negative
  signing, and prints `codesign -d --entitlements - <helper>` after each;
- requires `--list-tests Process.loadsAndCallsThroughARealHelper` to report `1 matching test case`;
- captures the run's output and requires a non-zero exit, no `No test cases matched`, and
  `^test cases: +1 \|( +0 passed \|)? +1 failed`;
- reads `sudo log show --start <just before the run> --predicate 'eventMessage CONTAINS "rejecting invalid
  page" AND eventMessage CONTAINS "contour-extensio"'` and requires a line with `in file "<nil>"`, and no
  rejection line without it (no stale-signature kill).

**What CI must show:** exactly one such `<nil>` line for `contour-extensio`. Two other outcomes are
possible:
- If the log is empty, the kernel line is not reaching the unified log on the runner. Look at the
  captured test output instead; the next fallback is the crash report under
  `~/Library/Logs/DiagnosticReports` (`Termination Reason: CODESIGNING 2 Invalid Page`).
- If the helper loads and runs, the hardened runtime is not enforcing as expected.

### 2. Profile test: exact allow table (Important)

`Sandbox_test.cpp` now parses the profile:
- `withoutComments()` drops `;` comments.
- `operationsOf(profile, action)` collects the operation words of every `(<action> …)` form, once per form,
  up to its first filter.

`Sandbox.profileHasNoNetworkOrMachLookup` requires:
- the profile starts with `(version 1)\n(deny default)`;
- the deny operations are exactly `{default}`;
- there is no `*` anywhere in the profile, comments included;
- the sorted allow operations equal the data table `AllowedOperations` = `{ipc-posix-shm-read-data,
  ipc-posix-shm-read-metadata, ipc-posix-shm-write-data, signal, sysctl-read}`. Because it compares lists,
  not sets, a second form for the same operation fails as well;
- each rule keeps its filter: `(ipc-posix-name-prefix (param "SHM_PREFIX"))`,
  `(allow signal (target self))` and `(sysctl-name-prefix "hw.optional.")`.

A new case, `Sandbox.profileParserSeesWhatAWildcardWouldHide`, tests the parser on profiles it must refuse
(`file*`, a second `signal`, `(allowed`, and a commented-out rule).

Mutation check: rewriting the shm rule to `(allow ipc-posix-shm* iokit-open …)` failed the test, listing
`allowed: iokit-open ipc-posix-shm* signal sysctl-read`. I then reverted it.

Both cases run on every platform, in `vtextension_remote_test`.

### 3. Deny log (Important)

- A new step, "Start of the sandbox log", writes `SANDBOX_LOG_START` to `$GITHUB_ENV` right before Tests.
- "Sandbox deny log" runs `sudo log show --start "$SANDBOX_LOG_START" --style compact --predicate 'sender ==
  "Sandbox" AND (eventMessage CONTAINS "contour-extensio" OR eventMessage CONTAINS "extension_host_p")'`,
  with no `tail` and no `grep`.

Names: the binaries are `contour-extension-host` (also `-hostile` and `-wamr`) and
`extension_host_probe`. The kernel's `p_comm` keeps 16 characters (`contour-extensio`,
`extension_host_p`), and `proc_name()` may give the longer `p_name`. Both forms contain the substrings
used, so the filter matches either.

### 4. sysctl comment (Minor)

The header doc and the SBPL comment now say:
- the CPU feature sysctls are read and cached while the first engine is created, which priming does before
  the lockdown;
- the rule is kept defensively, and is to be removed once a green run's deny log shows nothing needs it.

The rule itself is unchanged.

### 5. Probe table (Minor)

The macOS table gains `Unasserted` rows for `send-descriptor`, `ptrace-traceme`, `spawn-thread` and
`protect-writable-executable`. The probe is not hardened-runtime signed, so I expect
`protect-writable-executable` to be *refused* by XNU's W|X check (an errno, not a kill). `Unasserted`
needs the probe to survive. If any of the four kills the probe, CI will say so, and that row becomes
`Killed`.

### 6. Unentitled helper refused (Minor)

`signing_commands()` raises `Failure` when a binary named like a `NESTED_ENTITLEMENTS` row
(`contour-extension-host`) is found anywhere other than at its row's path. The self-test moves the helper
to `Contents/MacOS/` and requires the refusal, naming the binary. It reports `8 cases OK`.

### 7. Wasmtime named explicitly (Minor)

The reconfigure is now `cmake --preset macos-ci -DCONTOUR_EXTENSION_HOST=out-of-process
-DCONTOUR_WASM_RUNTIME=wasmtime`. A missing Wasmtime then fails at configure with the existing
`FATAL_ERROR` ("CONTOUR_WASM_RUNTIME is wasmtime, but no Wasmtime can be used: …").

### Checks (HEAD before the message amend; the amend changed no file)

- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment|macos_bundle'`: 9/9 passed.
- `vtextension_remote_test "Sandbox.*"`: 7 cases, 6 passed and 1 skipped (`systemWithoutOneRunsUnsandboxed`);
  76 assertions.
- `CHECK_WORKFLOWS_REQUIRE_TOOL=1 CHECK_SPELLING_REQUIRE_TOOL=1 ctest -L lint`: 34/34.
  `scripts/check-workflows.py` reports `actionlint: workflows OK (8 files)`.
- `python3 -I scripts/macos-bundle.py --self-test`: 8 cases OK.
- tidy-diff (`--base 71cbaa680`, `JOBS=4`): 6 translation units clean, after fixing one finding
  (readability-container-contains in the new parser test).
- clang-format 22, editorconfig and typos: clean.
- Not re-run this round: `vtextension_process_test "[sandbox]"` on Linux. Only the macOS-guarded table
  changed there; the target rebuilt cleanly and is in the 9/9 above.

### Concerns (this round)

- The negative case now depends on the kernel's `CODE SIGNING … rejecting invalid page` line being visible
  to `sudo log show` on GitHub's runners. It almost certainly is, because kernel `printf` goes to the
  unified log, but it has not been observed here.
- I am inferring that the unentitled helper dies by code-signing kill rather than by a failing `mprotect`,
  by reading current XNU and Wasmtime source. The first CI run confirms or refutes it, and the step's
  output shows which.

### Addendum: GCC `-Werror=logical-op` (docs job)

`errno == EAGAIN || errno == EWOULDBLOCK` compares the same value twice wherever the two are equal (Linux,
macOS, the BSDs), and GCC refuses that. The branch's diff against `origin/master` has three such pairs:
`Channel_posix.cpp:219` and `WakeSignal_posix.cpp:119` and `:160`. There are no others.

- New header `src/vtextension/wasm/remote/transport/WouldBlock.hpp`, listed in `vtextension_remote_wire`'s
  sources, with `[[nodiscard]] constexpr bool wouldBlock(int error) noexcept`. It compares EWOULDBLOCK only
  `#if EWOULDBLOCK != EAGAIN`. All three sites now call `wouldBlock(errno)`.
- Each fix is folded into the commit that introduced its lines, according to `git blame` and `git log -S
  EWOULDBLOCK`. Those are not `9e44f07fa`, as the coordinator's note had it:
  - `0c8d71383` (inheritable channels, an interrupt page and a wake signal) → now `a14efb900`. It gets the
    header, the CMake line and the two WakeSignal sites.
  - `5fca3494a` (let go of an extension host without waiting for it) → now `61eb74b56`. It gets the
    Channel site.
- The autosquash rebase (onto `origin/master` `8c3afaa0c`) had two include-block conflicts in
  `Channel_posix.cpp`, at `5fca3494a` and `df0562eb5`. Both were resolved by keeping both includes.
  `git diff <pre-rebase fixup head> HEAD` is empty. In the range-diff, `19a2bf6d1` and `8c6adcbe8` changed
  only in CMake context.
- New hashes of this task's commits: `f421c11ff` (Item 0), `4a8546187` (sandbox), `9fb129082` (entitlement).
  **HEAD `9fb129082`.**

Verification:
- **GCC 16.2.1:** both translation units, compiled by hand with `g++ -std=c++23 -fsyntax-only -Wall -Wextra
  -Wlogical-op -Werror` and the `-I`/`-D` flags from `compile_commands.json`, are clean. The pre-fix
  `Channel_posix.cpp` reproduces the CI error (`219:33: error: logical 'or' of equal expressions
  [-Werror=logical-op]`).
- **clang-tidy:** run directly on both translation units; clean.
- **Formatting:** clang-format and editorconfig are clean.
- **Rebuild:** the clang-asan rebuild of every vtextension target came back with 0 warnings.
- **Tests:** `ctest -R 'vtextension|wasm_runtime_containment'` passed 8/8, and `ctest -L lint` (with both
  require-tool variables set) passed 34/34.
- **Rewritten intermediate commits:** not rebuilt in full. I checked by reading that the header exists, and
  is included where it is used, at `a14efb900`, `61eb74b56` and `b5078580f`.
