# Task 4.7 report: gate

Status: **DONE_WITH_CONCERNS** (concerns are about unproven platforms only). Branch
`feature/out-of-process-extension-host`, base `3a49108ff`, head `c326cc652`. Nothing pushed. Step 7 is the
controller's; I stopped after Step 6 and the Step 8 draft below.

## Commits

| Hash | Subject |
|---|---|
| `16382d4c5` | refactor(vtextension): read guest memory through one function the hostile test shares (Item 0.1) |
| `aa6ddaacf` | test(vtextension): flat lifts fetch each guest byte once too (Item 0.2) |
| `63dbffe76` | test(vtextension): pick the probe's flips from the engine's raw output (Item 0.3) |
| `b81ad5a18` | docs(vtextension): say what a request failing before the host claims a grant means (Item 0.4) |
| `7b23b0cdd` | test(vtextension): measure copied memory, instantiation and shared pages (Step 3) |
| `c326cc652` | fix(vtbackend): trim only ASCII whitespace from a line's text (found in Step 4) |

All `git commit -s`, no AI trailers, no plan/task/phase references.

## Timestamps

| Step | Start | End |
|---|---|---|
| 0 (Item 0) | 2026-11-25 16:49:50 | 16:56:09 |
| 1 ASan | 16:55:16 | 16:57:09 |
| 2 TSan | 16:57:16 | 16:59:57 |
| 3 Measurement | 17:02:09 | 17:02:57 (measure code 16:59–17:02) |
| 4 Windows + wine | 17:03:08 | 17:14:13 (includes the vtbackend fix) |
| 5 macOS/FreeBSD reading | 17:14:20 | 17:15:59 |
| 6 Coverage | 17:16:03 | 17:19:23 |
| 8 Summary draft | 17:19:23 | 17:23 |

## Item 0

1. **Seam:** new internal header `src/vtextension/GuestMemoryReads.hpp` with `detail::readOnce(memory, address,
   into)` (the size check + one memcpy from `writable()`; a template, so a `final` memory calls `writable`
   directly) and `detail::guestRange(expected<span, MemoryFailure>)` (OutOfBounds → Trapped, HostCrashed →
   HostUnavailable). `Instance::GuestMemory::read/writable` are now one line each over them; `HostileMemory_test`'s
   `ScribbledMemory` calls the same two over `SharedGuestMemory::view`, its copy of the logic is gone. Building the
   whole `Instance::GuestMemory` over a `SharedGuestMemory` would have meant decoupling it from `Instance`
   (allocate calls `cabi_realloc`, the budget is the instance's limits) — the read path is what the test needs.
2. `GuestReader.flatLiftsFetchEachByteOnce` (TEMPLATE_LIST over all 31 `LiftedTypes`): `lowerFlat`, decoy on,
   `liftFlat`; value equal and no byte read twice. Seen failing: `readBytes` reading through `writable()` → 23
   assertions fail; restored.
3. The probe derives offset (`random() % size`) and mask (`random() % 0xFF + 1`) from raw `mt19937` output; the
   comments now say a seed names the same flips on every standard library and the interleaving stays the
   scheduler's.
4. Comment fixed as asked.

ASan after Item 0: `GuestReader.*` 63 cases / 337 assertions; `hostile memory does not break decoding` passes;
clang-tidy clean on every changed `.cpp`; clang-format 22 and editorconfig-checker clean.

## Step 1: ASan/LSan (`out/clang-asan`)

Built the five test binaries and the three executables (`contour-extension-host`, `-hostile`,
`extension_host_probe`), warning-free.

- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: **7/7 passed** (42 s); again 7/7 after
  the vtbackend fix.
- `vtextension_test` 473 cases / 25924 assertions; `vtextension_wasmtime_test` 63 (61 + 2 Signals skips);
  `vtextension_remote_test` 291 / 12090; `vtextension_process_test` 105 (104 + 1 skip); `vtextension_helper_test
  "~[.measure]"` 158 (157 + 1 skip).
- Contract: 42 cases per composition (vtextension_test 42, wasmtime 42, remote 84 = Shared + Copy, helper 42).
- `uncleanExits == 0`: checked by `checkHelpersEndedCleanly` in Helper_test (passes).

## Step 2: TSan (`out/clang-tsan`, already configured with the same options)

Same eight targets built warning-free. `ctest --preset=clang-tsan -R 'vtextension|wasm_runtime_containment' -V`:
**7/7 passed**, `grep -c ThreadSanitizer` on the full verbose log = **0**. No `TSAN_OPTIONS`, no suppressions.
Counts as in ASan (vtextension_test 473/25924, remote 291/12090, helper 158 with 1 skip, process 105 with 1 skip,
wasmtime 63 with 2 skips — the Signals cases skip in instrumented builds).

## Step 3: measurement (`out/clang-release`)

New in `ProcessMeasure_test.cpp` (commit `7b23b0cdd`): `Process.measureRender` gains a section through
`--hostile=copy-memory` (the real host with `makeCopyingHelperEngine`; that row has no incident, so it serves
directly, no relay); new `Process.measureInstantiate` (plasma, 200 `Instance::create`s after one warm-up, each
released); the footprint adds RssAnon, RssShmem, Pss (smaps_rollup) and the parent's own footprint.

**The helper's Pss cannot be measured:** the helper makes itself non-dumpable (hardening), so its
`/proc/<pid>/smaps_rollup` is closed to the parent; `status` stays readable. The test prints "not readable"
instead of failing. RssShmem shows the shared pages instead.

Built `vtextension_helper_test vtextension_remote_test vtextension_wasmtime_test` (three: the in-process Wasmtime
twin lives in the wasmtime suite). Three sequential runs, unniced, `uptime` load 1.65–1.9 on 32 threads (another
session building; quiet enough — steady medians spread ≤ 0.9 µs).

| Measure | run 1 | run 2 | run 3 | **median** | Phase 3 |
|---|---|---|---|---|---|
| Process steady (shared) | 8.4 | 8.3 | 9.2 | **8.4 µs** | 18.1–18.6 |
| Process (copy) steady | 18.4 | 19.2 | 18.7 | **18.7 µs** | (= phase 3's path) |
| Remote (in-process host, shared) steady | 9.0 | 9.0 | 8.6 | 9.0 µs | |
| Wasmtime in-process steady | 3.3 | 3.4 | 3.4 | 3.4 µs | |
| Process uncached (shared) | 25.5 | 18.3 | 26.5 | 25.5 µs | 25–40 |
| Process (copy) uncached | 26.4 | 25.7 | 38.3 | 26.4 µs | |
| Remote uncached | 20.0 | 18.8 | 20.1 | 20.0 µs | |
| Wasmtime uncached | 12.9 | 12.8 | 13.1 | 12.9 µs | |
| Process round trip | 2.7 | 3.3 | 5.0 | **3.3 µs** | 3.3 |
| Remote round trip | 2.9 | 3.2 | 3.3 | 3.2 µs | |
| Spawn to runtime created | 1.27 | 1.28 | 1.29 | **1.28 ms** | 1.2 |
| Instantiate plasma, shared | 16.0 | 18.0 | 21.0 | 18.0 µs | new |
| Instantiate plasma, copy | 15.9 | 11.3 | 12.5 | 12.5 µs | new |
| Helper idle VmRSS | 10.6 | 10.6 | 10.8 | **10.6 MiB** | 10.8 |

After 16 plasma instances (identical across runs to ±0.5 MiB): helper VmData 46.9 MiB (46.4 after load), VmRSS
23.0 (RssAnon 5.4, RssShmem 0.7), Pss not readable; parent (the test process) VmData 17.1, VmRSS 10.5 (RssShmem
0.7), Pss 4.2 MiB.

- **R31 gate: passes.** Steady median 8.4 µs ≤ 18.6 µs (even below D83's original 18.1).
- **Copy vs shared:** 18.7 − 8.4 = **10.3 µs saved per steady frame (55 %)**; uncached 26.4 vs 25.5 (within
  noise: an uncached frame is dominated by the guest's work).
- **Overhead (helper − in-process Wasmtime) / helper:** steady (8.4 − 3.4)/8.4 = 60 %; uncached
  (25.5 − 12.9)/25.5 = 49 %. The out-of-process helper costs about 5 µs per steady frame over in-process.
- **Instantiate:** sharing adds ≈ 5.5 µs per instance (memfd + ftruncate + seals + parent map, grant, helper map).
- **D81 measured:** 16 objects add ~0.5 MiB to the helper's VmData; the `MAP_SHARED` guest memory itself is not
  counted by RLIMIT_DATA (it appears as RssShmem).

Recorded in the plan's Phase-4 facts ("From 4.7").

## Step 4: Windows cross-build + wine

Cross-build (`out/clangcl-debug`: clang-cl, Debug, Embedded debug info, `-Werror`, extensions on, bundled off, GUI
off): `vtextension_process_test extension_host_probe contour-extension-host contour-extension-host-hostile
vtextension_helper_test vtextension_remote_test`. **First attempt failed on my own Step 3 code**
(`-Wunused-function readProcFields`, `-Wunused-const-variable MeasuredInstantiations` — Linux/bundled-only
helpers); guarded and folded into `7b23b0cdd` (unpublished, my own commit). Then clean, including the 4.6
additions (probe `--mutate`, `HostileMemory_test`).

wine 11.20, own prefix `scratchpad/wine47`, `WINEPATH` = mingw `zlib1.dll`, debug CRT DLLs in bin. Five runs:

| Run | Result |
|---|---|
| `vtextension_process_test.exe` (full) | 98 cases: 97 passed, 1 skipped (the wine-skipped cross-view case) / 716 assertions |
| `vtextension_remote_test.exe` (full) | 262 cases: 260 passed, 2 skipped / 11241 assertions |
| `vtextension_helper_test.exe "~[.measure]"` | 128 cases: 126 passed, 2 skipped / 3035 assertions |
| `vtextension_helper_test.exe "hostile memory does not break decoding,Hostility.copyingHostServes"` | 2 cases / 29 assertions, passed (probe `--mutate` under wine; Copy path end to end) |
| `vtextension_process_test.exe "HandleGrant.*,SharedObject.*,GuestMemoryMapping.*,Channel.parentInstallsNoDescriptorFromTheHelper"` | 18 cases: 17 passed, 1 skipped / 150 assertions |

`wineserver -k` on my own prefix only.

**Real break found: the MSVC debug CRT asserted** (`isctype.cpp(36): Assertion failed: c >= -1 && c <= 255`,
twice) in `ExtensionHost.unloadableNoticeIsDrawn`: `Line::toUtf8Trimmed` and `Grid::lineTextTrimmed` passed each
`char` of UTF-8 text to `std::isspace`, so the "⚠" of the notice reached it as negative values (UB; on glibc a
Latin-1 locale would also call 0xA0 a space and cut U+26A0 in half). Pre-existing vtbackend code, not phase-4.
Fixed in `c326cc652`: `vtbackend::isAsciiWhitespace(char)` (constexpr, Line.hpp) used by both. Test
`Line.toUtf8TrimmedKeepsMultibyteText` (U+00A0, x, U+26A0; static checks). It cannot fail on Linux with the C
locale; the proof is wine: after the fix the same case under wine prints no assertion (3 assertions, passed).
`vtbackend_test` (ASan) whole: 3623 cases / 246977 assertions passed; clang-tidy clean on the three files.

**What wine showed:** clean `-Werror` compiles; `SEC_RESERVE` create/commit; `DuplicateHandle` into a child
(HandleGrant.*); the shared path with placeholders (`canMapGuestMemory()` is true under wine, as in 4.4); the
Copy path end to end (`copy-memory`); the probe's mutate path.
**What wine cannot prove:** real placeholder behaviour on Windows; an access violation on uncommitted pages
becoming a trap; job accounting of section commits; cross-view commit semantics
(`SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother` skips under wine by design).

## Step 5: FreeBSD/macOS — reading checklist (no push allowed; CI outstanding)

| Item | Finding |
|---|---|
| XNU `ftruncate` works once | `createSharedObject` sizes the object once, right after `shm_open(O_CREAT|O_EXCL)` + `shm_unlink`; never again (macOS has no seals; the `#ifndef __APPLE__` skips `F_ADD_SEALS`). Name `/contour-eh-<pid>-<serial>` stays under PSHMNAMLEN 31 for any 5-digit pid and serial < 10^13. OK. |
| macOS has no `MSG_CMSG_CLOEXEC` | `Channel_posix.cpp`: `ReceiveFlags = 0` under `#ifndef MSG_CMSG_CLOEXEC`, then `fcntl(F_SETFD, FD_CLOEXEC)` per received descriptor; failure → `ChannelError::Io` with every received descriptor owned and closed. The window between `recvmsg` and `fcntl` only matters if the helper forked meanwhile; it does not. OK. |
| `CMSG` alignment | Both control buffers are `alignas(::cmsghdr) std::array<std::byte, CMSG_SPACE(...)>`; walk uses `CMSG_FIRSTHDR/NXTHDR`, count from `cmsg_len - CMSG_LEN(0)` (12 on macOS, 16 on FreeBSD amd64, both right), data copied with `memcpy` (no unaligned int load). **Risk:** `CMSG_SPACE` is a `std::array` bound, which needs a constant expression; FreeBSD's `_ALIGN` is integer-only, but if a macOS SDK's `__DARWIN_ALIGN32` casts through a pointer, macOS fails to compile. Not checkable here (no SDK); first macOS CI run decides. |
| shm `MAP_FIXED` over an anonymous reservation | `GuestMemoryMapping_posix.cpp`: `PROT_NONE MAP_PRIVATE|MAP_ANONYMOUS` reservation of object + guard, object `MAP_SHARED|MAP_FIXED` at its base, tail `mprotect(PROT_NONE)`. All offsets are 64 KiB multiples, so 16 KiB-page aligned on arm64 macOS; no `MAP_NORESERVE` (absent on FreeBSD). One `munmap` releases both. OK by reading. |
| The Rosetta bound | `fitsSharedObject` accepts up to `roundUp(expected, 16 KiB)` whatever the reader's page; guest objects are 64 KiB multiples (no rounding), the control page may be rounded by an arm64 parent and is accepted by an x86_64 helper seeing 4 KiB pages. OK by reading. |
| FreeBSD memfd seals, parent-only role | `memfd_create(MFD_CLOEXEC|MFD_ALLOW_SEALING)` (FreeBSD ≥ 13), `F_ADD_SEALS SHRINK|GROW` then `SEAL`; `F_SEAL_FUTURE_WRITE` is `#ifdef`'d (absent on FreeBSD: the interrupt page relies on the helper's read-only map). Only the parent creates and seals; the helper only maps/mprotects received descriptors. `canMapGuestMemory()` is `true` on all POSIX; the FreeBSD helper's engine (WAMR, 5.3) must say Copy in its Hello if it cannot provide memory — a 5.3 input. |

**Outstanding for CI (owner has not allowed a push):** FreeBSD and macOS builds of the whole phase (SharedObject,
Channel recvmsg path, GuestMemoryMapping, the macOS `CMSG_SPACE` question above);
`PduStream.descriptorGoesWithItsFrame` and `WasmContract.instantiatesConcurrently` on macOS/FreeBSD (the BSD
`attachedFrameStart` branch is verified by reading only); the real-Windows cross-view commit test; Windows
placeholders / access-violation traps / job accounting on real Windows.

## Step 6: coverage (`out/clang-coverage`, five suites + helpers, llvm-cov gcov, fresh `.gcda`)

`ctest --preset=clang-coverage -R 'vtextension|wasm_runtime_containment'`: 7/7 passed.

| Source | Lines covered |
|---|---|
| transport/SharedObject.cpp | 69.6 % of 23 |
| transport/SharedObject_posix.cpp | 88.5 % of 61 |
| transport/HandleGrant_posix.cpp | 100 % of 15 |
| transport/GuestMemoryMapping.cpp | 76.3 % of 38 |
| transport/GuestMemoryMapping_posix.cpp | 88.2 % of 34 |
| host/MemoryProvision.cpp | 100 % of 8 |
| wasmtime/WasmtimeMemoryCreator.cpp | 95.2 % of 42 |
| remote/SharedGuestMemory.cpp | 82.3 % of 62 |
| remote/RemoteBackend.cpp | 95.1 % of 450 |
| host/HostServer.cpp | 94.9 % of 473 |
| **All together** | **92.9 % of 1206** |

Kinds of uncovered lines:
- **Move-assignment of the RAII mappings** (`SharedMapping`, `GuestMemoryMapping`): all of SharedObject.cpp's and
  GuestMemoryMapping.cpp's misses.
- **Syscall-failure branches** and their errno mapping: `memfd_create`/`shm_open`, seals, `fstat`, `mmap`,
  `mprotect` failing (SharedObject_posix, GuestMemoryMapping_posix); `SharedGuestMemory::create`'s error mapping
  of those; Windows-only commit refusal (`grow` → `Refused`, SharedGuestMemory.cpp:112).
- **The creator's defensive grow guards** Wasmtime never triggers (`newBytes <= accessible`, `> object`).
- **RemoteBackend:** host death in the middle of a request (send failures, `crashed()` returns), grant / create
  failure during instantiate, a crashed-host call failure mapped to Trap.
- **HostServer:** protocol-violation branches for malformed parent PDUs (no test sends the helper bad parent
  frames), Grow/Grown send failures, a Release of a `Runtime` target, interrupt of a slot mid-call.

Worth a test later: a scripted malformed-parent-PDU row for HostServer (already listed in phase 3).

## Deviations

1. Item 0.1 shares the **read path** (`readOnce`, `guestRange`) rather than constructing the whole
   `Instance::GuestMemory` over a `SharedGuestMemory` (it is bound to its `Instance` for allocation and budget).
2. Step 3 needed new measure code (copy-memory render, `measureInstantiate`, Pss/RssShmem, parent footprint); its
   own commit. Built three release targets, not two (the in-process Wasmtime twin is in the wasmtime suite).
3. Helper Pss is "not readable" by design (non-dumpable helper); RssShmem reported instead.
4. The vtbackend `isspace` fix is outside the extension work; it is the one real break the gate found.
5. `/simplify` and `/code-review` not run (they dispatch agents; Step 7 is the controller's).

## Concerns

1. macOS: `CMSG_SPACE` as an array bound may not be a constant expression on some SDKs (unverifiable here).
2. All BSD/macOS and real-Windows behaviour is still by reading and wine only; CI outstanding (above).
3. Uncached render through a process stays noisy (18.3–26.5 µs shared across three runs).

## Phase 4 summary

**What the phase did.** Guest memory is one shared object per instance, created and sized by Contour, granted to
the helper with the `Instantiate` (POSIX `SCM_RIGHTS`, Windows `DuplicateHandle`), mapped by the helper as
Wasmtime's provided memory, grown only through `Grow`/`Grown` answered by Contour, and read by Contour bounded by
its own acknowledged size, decoding every lifted value from one copy.

### Performance (release, medians of three runs; vs phase 3)

| Measure | Phase 3 | Phase 4 | Change |
|---|---|---|---|
| Plasma steady render, real helper | 18.1–18.6 µs | **8.4 µs** | −55 % (R31: pass) |
| Same, copying helper | — | 18.7 µs | the phase-3 path, unchanged |
| Plasma uncached, real helper | 25–40 µs | 25.5 µs | within noise |
| In-process shared host / Wasmtime steady | — | 9.0 / 3.4 µs | |
| Overhead (helper − Wasmtime)/helper | — | 60 % steady, 49 % uncached | |
| Round trip | 3.3 µs | 3.3 µs | = |
| Spawn to runtime | 1.2 ms | 1.28 ms | ≈ |
| Instantiate plasma | — | 18.0 µs shared, 12.5 µs copy | +5.5 µs per instance |
| Helper idle VmRSS | 10.8 MiB | 10.6 MiB | = |
| Helper after 16 plasma | VmData 63.1 MiB* | VmData 46.9, VmRSS 23.0 (Shmem 0.7) | guest memory left VmData (D81) |

\* phase 3's figure, measured with copied memory; the drop is the guest memory leaving the private heap.

### Risk

- **D69 (handle passing on control with Instantiate).** Proven on Linux (ASan, TSan, release) and Windows under
  wine (DuplicateHandle into a child). The Linux receive-grouping bug found in 4.5 is fixed and pinned
  (`PduStream.descriptorGoesWithItsFrame`); the BSD grouping branch is by reading only — a wrong assumption there
  shows as a protocol violation (safe, loud), not silent misuse. Lost grants die with the helper; the parent
  installs nothing the helper sends (`Channel.parentInstallsNoDescriptorFromTheHelper`, `descriptor-up`).
  Residual: the macOS `CMSG_SPACE` constant-expression question.
  **R38 (phase review):** a descriptor the kernel merely discards is closed in the receiving parent thread, and
  a last close can block without bound (a lingering socket). Contour's ends now set `SO_PASSRIGHTS = 0`
  (Linux ≥ 6.16; this box 7.2): the helper's `sendmsg` fails `EPERM` and no descriptor enters Contour at all
  (`Channel.parentEndRefusesDescriptors`). Remaining exposure: Linux < 6.16, FreeBSD and macOS still discard
  (noted once at debug level; `ChannelPair::upward == Discarded`). Windows carries no handles up.
- **D74 (Windows placeholders).** wine supports `VirtualAlloc2`/`MapViewOfFile3`, so the shared path runs there;
  real Windows placeholder behaviour, uncommitted-page access violations becoming traps, and cross-view commits
  are CI's. If placeholders are missing the Hello says Copy and the copy path serves (proven under wine via
  `copy-memory`). Medium risk until Windows CI runs.
- **D75 (provision through a thread-local, set by HostServer's control thread).** Exercised by every Shared
  instantiation in all suites; a missing or second memory is `InvalidComponent`. Low risk.
- **D76 + R29 (growth).** Parent checks (innermost frame id, whole pages, `acknowledged < n ≤ min(cap, object)`)
  are pinned by `growthIsChecked`, `badGrowKillsTheHost`, `badGrowDuringInstantiateKillsTheHost`, the grow
  hostility rows, growth during lowering, while interrupted and in a start function. Bounds on Wasmtime/Signals
  are the closed pages themselves (4.4 fact) — so real Windows access-violation handling is the remaining unknown.
- **D81 + R30 (memory outside RLIMIT_DATA).** Measured: guest memory is RssShmem, not VmData. R30 caps the sum of
  live objects per incarnation at `dataBytes` (1 GiB) — `liveObjectsStayWithinTheDataCap`. Accepted residuals:
  a compromised helper can dirty its live objects up to that cap, keep released objects mapped until it dies,
  and on Windows commit section pages through its own view (counted by the job memory limit, unverified on real
  Windows).

### Coverage

Phase-4 sources 92.9 % of 1206 lines (per-source table in Step 6). Uncovered: RAII move-assignments, syscall
failures, never-triggered creator guards, host death mid-request, malformed parent PDUs.

### Tests

ASan and TSan: `vtextension|wasm_runtime_containment` 7/7 (TSan: zero reports, no suppressions); 42 contract
cases per composition; `uncleanExits == 0`. vtbackend_test whole passes after the trim fix.

### What wine proved / did not

Proved: `-Werror` clean Windows compiles of every phase-4 target (and the 4.6 additions); `SEC_RESERVE`
create/commit; `DuplicateHandle` grants into a child; the shared path through wine's placeholders; the Copy path
end to end; the hostile mutate probe; and a real MSVC-debug-CRT bug in vtbackend, now fixed. Did not prove: real
Windows placeholders, access violations on uncommitted pages becoming traps, job accounting of section commits,
cross-view commits.

### CI outstanding (no push allowed)

FreeBSD and macOS builds and runs of the whole phase, notably `PduStream.descriptorGoesWithItsFrame` and
`WasmContract.instantiatesConcurrently` on both, and the macOS `CMSG_SPACE` bound; real-Windows
`SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother` and the placeholder/trap/job items above.

## Phase review fix wave

Findings: 0 Critical, 1 Important, 2 Minor. One commit per item.

| Commit | Item |
|---|---|
| `2126634ae` | fix(vtextension): make Contour's channel ends refuse descriptors a host sends (Important, R38) |
| `b52650093` | fix(vtextension): try a fresh shared memory name on macOS when one is taken (Minor) |
| `009b13c5e` | test(vtbackend): say which runtimes the multibyte trimming check bites on (Minor) |

**1. Upward descriptors (R38).**
- `UpwardDescriptors { Discarded, Refused }` and `ChannelPair::upward` (Channel.hpp, documented at the
  declaration). `makeChannelPair()` (Channel_posix.cpp) sets `SO_PASSRIGHTS` to 0 on the **parent end only**,
  under `#ifdef __linux__`. The value is 83, checked in `/usr/include/asm-generic/socket.h`, and is used as a
  fallback when the libc headers lack it. `ENOPROTOOPT`/`EINVAL` → `Discarded`; any other failure → `Io`. Windows
  pairs say `Refused`, since no handle travels beside a pipe's bytes.
- `sendWithHandle` maps `EPERM` to `ChannelError::Refused`, with nothing sent.
- `ProcessHostConnector` notes `Discarded` once per process, through a new disabled-by-default category
  `vtextension.host.connection` (debug, no user-facing text). The std-only transport stays log-free.
- Hostile `descriptor-up`: when the send is `Refused`, the relay forwards the frame bare (`Verdict::Forward`), so
  the host serves on. The row's assertions are unchanged (sum 5, no deaths, one launch, descriptor count
  unchanged) and pass; its comment now names both kernels.
- Tests:
  - New `Channel.parentEndRefusesDescriptors` (Linux; SKIPs on kernels before 6.16, did not skip here): the
    helper's `sendWithHandle` is `Refused`; a following plain send arrives as only its own 3 bytes with no
    handle; the helper's end still receives a grant. Checked against a mutation that skips the `setsockopt`
    (the case fails), then restored.
  - `Channel.parentInstallsNoDescriptorFromTheHelper` now runs on `discardingPair()` (raw `socketpair` +
    `Channel::adopt`, so neither end refuses). It still proves the plain-recv discard path the BSDs and older
    kernels use.
- clang-tidy clean on Channel_posix.cpp, Channel_test.cpp, ProcessHostConnector.cpp, HostileHost.cpp,
  Hostility_test.cpp.

**2. macOS shm name squatting.**
- New std-only `transport/SharedObjectName.hpp`:
  - `sharedObjectName(pid, suffix)` → `/contour-eh-<pid>-<12 hex>`, at most 30 characters for pid 99999.
  - `createUnderFreshName(pid, nextSuffix, create)` tries up to 8 names while `create` returns `EEXIST`, and
    stops at any other result.
- The macOS branch of `openObject` uses it, with 64 random bits from `std::random_device` per name (48 kept), and
  unlinks the name once open. The serial counter is gone.
- Tests `SharedObject.nameFitsMacOS` and `SharedObject.takenNamesAreTriedAgain` (free at the third try, all 8
  taken → EEXIST, EACCES not retried). Checked against a no-retry mutation (fails), then restored.
- The macOS branch itself is uncompiled here. Its exact text was syntax-checked on Linux in a scratch
  translation unit (Linux has `shm_open`): it compiles.
- clang-tidy clean, after making the template take `const&` rather than unforwarded forwarding references.

**3.** The comment was added. The two code points are now spelled `\u00A0`/`\u26A0` escapes: the earlier
version had a literal, invisible no-break space. Test-only commit.

**Gates.**
- ASan: `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` **7/7 passed** (42 s).
  `vtextension_test "Channel.*,PduStream.*"` 28 cases / 605 assertions; `vtextension_process_test
  "SharedObject.*"` 8 / 56; `Hostility.descriptorUpIsNeverInstalled` passed; `vtbackend_test` Line case passed.
- TSan: `vtextension_test "Channel.*,PduStream.*"` 28 cases / 605 assertions, 0 ThreadSanitizer reports.
- Windows cross-build (`out/clangcl-debug`, `-Werror`): process, remote and helper tests, host, hostile host and
  probe all clean. wine `vtextension_process_test.exe` full: 100 cases (99 passed + the cross-view skip) / 725
  assertions.
- clang-format 22 and editorconfig-checker clean on every changed file.
- Nothing pushed.
