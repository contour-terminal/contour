# Task 4.5 report: parent side — an object per instance, the acknowledged size, answered growth

Status: **DONE**, with the deviations below. Branch `feature/out-of-process-extension-host`, base `891522849`. Nothing pushed.

## Commits

| Hash | Subject |
|---|---|
| `9efa54729` | fix(vtextension): trust only opened bytes after a granted growth (Item 0) |
| `67101b271` | fix(vtextension): give a granted descriptor to the frame it travelled with (bug found here, in 4.3 code) |
| `01008642a` | feat(vtextension): share guest memory with the extension host instead of copying it |
| `41aeac18e` | test(vtextension): hostile growth, descriptors and memory from an extension host |

All `git commit -s`, no AI trailers. Each of the last three was built and `ctest -R vtextension` run on its own tree
before it was committed (the stream fix: focused `vtextension_test "PduStream.*,Channel.*"` only, as it touches one
class; the full suite ran on every later tree).

## Item 0 (`9efa54729`)

1. `WasmtimeMemoryCreator.cpp` `growMemory`: after a Granted answer, `accessibleBytes() < newBytes` fails the
   growth ("granted but not opened"). Test: new section `"granted without opening"` in
   `WasmtimeEngine.growthAsksAndOpens`, with a GrowRequest that answers Granted and opens nothing: `memory.grow`
   returns -1 and `memory(64 KiB, 1)` is OutOfBounds. Seen failing with the check disabled (`1 == 0xFFFFFFFF`).
2. `HostServer.cpp` `growOnCurrentChannel` doc comment rewritten so clang-format leaves it whole.
3. **Deviation:** `CATCH_CONFIG_NO_POSIX_SIGNALS` as a target definition cannot work: Catch2 v3 is a compiled
   library here (`/usr/lib64/libCatch2.so`, 3.13), and its fatal-condition handler is compiled into it. Instead,
   `WasmtimeEngine_test.cpp` gains `EngineSignalHandlers` (POSIX): made once a Signals case's instance exists, the
   first records Wasmtime's handlers, every later one installs them over Catch2's for its scope and restores
   Catch2's after. **Measured:** Wasmtime 48 installs its handlers lazily, by the first instantiation, *not* at
   engine creation (a guard made right after the engine recorded Catch2's handler and the second case still died).
   The Signals case is split into `signalTrapsPastTheOpenBytes` and `signalTrapsPastTheObject`, so the binary now
   has two Signals cases. Release tree (`out/clang-release`): both pass in three random orders; the whole binary
   passes (63 cases). Without the guard the second case dies (`SIGSEGV` reported by Catch2) — seen. No other test
   target creates a Signals engine (`makeHostedEngine` traps in software).

## The stream fix (`67101b271`) — not in the brief

Turning Shared on made `WasmContract.instantiatesConcurrently` fail 2–4 of 25 instantiations with
`HostUnavailable`. Cause: Linux's `unix_stream_read_generic` ends a receive with the skb carrying `SCM_RIGHTS` but
*begins* it with bytes queued before it. A Release sent just before an Instantiate arrived in the same
`recvmsg` as the Instantiate's descriptor; the server then saw "a Release that brought a handle" and ended as a
violation. 4.3's concern 1 had assumed the opposite grouping.

`PduStream` now records which frame a descriptor came with: on Linux the last frame that starts in the buffered
bytes at receipt, on the BSDs (which end a receive *before* such a message) the first that starts in the chunk.
`takeAttachedHandle()` hands it out only once that frame's PDU was received. Test
`PduStream.descriptorGoesWithItsFrame` (Release, Release, Instantiate+fd, Release sent before the reader reads):
seen failing with the attribution disabled (2 CHECKs). The BSD branch is read-only (uncompiled here).

## What was built

- `remote/SharedGuestMemory{.hpp,.cpp,_test.cpp}`: `GrowVerdict { Violation, Refused, Granted }`,
  `growStatusOf()`, `SharedGuestMemory::create(cap, initial)` (`OutOfMemory` for initial past the cap / not whole
  pages / WrongSize; `RuntimeUnavailable` otherwise), `object()`, `closeObject()`, `grant(wire)`, `view(addr, size)`
  (bounded by the acquire-loaded acknowledged size), `grow(bytes)`, `refuseGrowth(bytes)`, `acknowledgedBytes()`,
  `objectBytes()`. Create = make (`GuestMemory`), map whole RW, seal, commit `[0, initial)`. Grow = check
  (whole pages, `> acknowledged`, `<= cap`, `<= object`) → commit (Windows) → store release → Granted; commit
  failure → Refused.
- `HostConnection::objectBytesCap` (default `DefaultObjectBytesCap` = 1 GiB; 0 = no cap). `ProcessHostConnector`
  sets it from `caps.dataBytes`.
- `HelperIncarnation`: `memoryMode()` (from Hello), `handleTarget()`, `request(pdu, step, grant, GrowAnswer)`,
  `leaseObjectBytes(bytes) -> optional<ObjectBytesLease>` (R30, lock-free CAS). The control exchange answers each
  `Grow` before the reply with `Grown`, under the control mutex (R29 corollary); a Violation kills and returns
  `Malformed`. The Hello no longer refuses a mode.
- `HostRefusal::MemoryMode` removed (`LastHostRefusal = Unsandboxed`), its line and test row with it.
- `RemoteBackend.cpp`:
  - `RemoteModule` keeps `ModuleMemory { initialBytes, MemoryExport }` read from its own bytes
    (`largestInitialMemoryPages`) and the described exports.
  - `instantiate`: initial past the cap → `OutOfMemory` before anything (both modes). Shared: lease (R30) →
    create → `grantHandle(host.handleTarget(), object)` → request with grant and a GrowAnswer (id must be the one
    being made) → `closeObject()`.
  - `RemoteInstance` holds `variant<CopyPathMemory, SharedInstanceMemory{memory, lease}>`; `memory()` is
    OutOfBounds for an unexported memory; `flushTo` does nothing for shared memory; the call loop answers `Grow`
    per D76 (`answerGrow`: not shared or wrong id → kill; interrupted → `refuseGrowth`; else `grow`).
- Switched on: `makeHelperEngine()` → Provided iff `canMapGuestMemory()`; new `makeCopyingHelperEngine()` (for the
  `copy-memory` row); `makeHostedEngine` defaults to `Shared`.
- Contract case `grownMemoryIsVisible` (42 cases now), using `GrowModule`.
- Hostile host: the relay forwards a granted descriptor with its Instantiate (POSIX `receiveSomeWithHandles` on
  control→helper, `sendWithHandle` on forward; Windows: the value is this process's, forwarded as is);
  `HostMemory { AsBuilt, Copied }`, `GrantedMemory { Untouched, Scribbled }`; rows `grow-past-cap`,
  `grow-shrinks`, `grow-unaligned`, `grow-wrong-instance`, `descriptor-up`, `memory-unusable`,
  `scribble-memory`, `copy-memory`. Hostile setup failure code 76 → 79 (76 is now the real host's
  MemoryUnusable).

## Tests, and how each was seen failing (mutation, then restored)

| Test | Mutation → result |
|---|---|
| `SharedGuestMemory.boundsAreContoursSize` | view bounded by objectBytes → fails |
| `SharedGuestMemory.growthIsChecked` (brief's rows + refuseGrowth + up to cap) | `> acknowledged` dropped → 3 CHECKs fail |
| `SharedGuestMemory.viewsSurviveGrowth` | acknowledged not stored → fails |
| `SharedGuestMemory.objectIsClosedOnceGranted` (extra) | not mutated |
| `SharedGuestMemory.commitFollowsAcknowledgement` (Windows) | wine only, passes; not mutated |
| `RemoteBackend.helloMaySayEitherMode` | Shared Hello refused again → fails |
| `RemoteBackend.sharedMemoryNeedsNoMemoryPdus` | not mutated (a regression would hang into `within()`'s abort) |
| `RemoteBackend.growIsAnsweredAndBoundsFollow` | acknowledged not stored → fails |
| `RemoteBackend.badGrowKillsTheHost` (4 rows) | instance-id check dropped → fails |
| `RemoteBackend.growWhileInterruptedIsRefused` | interrupt ignored → fails (Granted, page reachable) |
| `RemoteBackend.initialSizeComesFromTheModule` | not mutated separately (past-cap section is also caught by `create`) |
| `RemoteBackend.unexportedMemoryIsOutOfBounds` | export check dropped → fails |
| `RemoteBackend.instancesHoldNoDescriptor` (1000 *live* instances, POSIX fds / Windows `GetProcessHandleCount`) | no `closeObject()` → `1019 == 19` |
| `RemoteBackend.growDuringLoweringIsSeen` (256 KiB through a growing `cabi_realloc`) | acknowledged not stored → fails |
| `RemoteBackend.instantiateHoldsControlWhileAStartFunctionGrows` | `sendReleases()` before `Grown` → fails |
| `RemoteBackend.liveObjectsStayWithinTheDataCap` | lease always granted → fails |
| `WasmContract.grownMemoryIsVisible` | acknowledged not stored → fails (Remote) |
| Hostile `grow-past-cap/-shrinks/-unaligned` | Violation verdict ignored → those 3 rows fail; `grow-wrong-instance` is caught by the id check |
| `Hostility.descriptorUpIsNeverInstalled` (POSIX) | parent lanes `Attachments::Taken` → `17 == 16` |
| `memory-unusable` row (Grow → CallCrashed, OnItsOwn, 1 unclean exit) | not mutated |
| `hostile memory through a real host` | not seen failing as such; it *did* catch a real tooling bug in release (below) |
| `Hostility.copyingHostServes` (extra) | not mutated |

The brief's "copy cases": `writesFlushBeforeCall`, `writesFlushBeforeHostResult`, `overlappingViewsSeePendingWrites`
now run over both modes, and the whole contract runs on `RemoteCopyTraits` (`makeHostedEngine(Copy)`) beside
`RemoteTraits` (Shared).

## Results

- ASan (`out/clang-asan`), final tree:
  - `ctest --preset=clang-asan -R vtextension`: **5/5 passed** (41.6 s).
  - `vtextension_test`: 410 cases / 25587 assertions; `[wasm-contract]` 42 cases.
  - `vtextension_wasmtime_test`: 63 cases (61 passed, 2 skipped = the two Signals cases); `[wasm-contract]` 42.
  - `vtextension_remote_test`: 288 cases / 12057 assertions.
  - `vtextension_process_test`: 105 cases (104 passed, 1 skipped).
  - `vtextension_helper_test "~[.measure]"`: 157 cases (156 passed, 1 skipped) / 3841 assertions.
- Release (`out/clang-release`, R22, same lock/nice): `vtextension_helper_test "~[.measure]"` 157 cases / 3854
  assertions all passed — the real helper with **Signals traps and Provided memory end to end** (contract 42,
  hostility, Review Focus, teardown). `vtextension_wasmtime_test` 63 cases all passed.
- Windows cross-build (`out/clangcl-debug`, `-Werror`): `vtextension_process_test vtextension_remote_test
  contour-extension-host contour-extension-host-hostile vtextension_helper_test extension_host_probe` clean.
- wine 11.20 (own prefix `scratchpad/wine45`, mingw `zlib1.dll` on `WINEPATH`):
  - `vtextension_remote_test.exe` full: 259 cases (257 passed, 2 skipped) / 11208 assertions — includes
    `commitFollowsAcknowledgement` and the 1000-live-instance handle count (the Windows self-grant to
    `currentProcess()` is claimed and closed by the in-process host).
  - `vtextension_helper_test.exe "~[.measure]"`: 127 cases (125 passed, 2 skipped) / 3015 assertions.
  - `vtextension_process_test.exe`: 98 cases (97 passed, 1 skipped).
  - After the last hostile fix: `vtextension_helper_test.exe "[hostility]"` 9 cases / 1496 assertions; remote
    `"SharedGuestMemory.*"` 5 cases.
- clang-tidy on every changed `.cpp` (Linux, `-p out/clang-asan`): clean after fixing three
  `math-missing-parentheses`. Windows (`-p out/clangcl-debug`) on `HostileHost.cpp`, `SharedGuestMemory_test.cpp`,
  `RemoteBackend_test.cpp`: one finding fixed; what remains is pre-existing (`Wire.hpp:189`, analyzer
  `NewDeleteLeaks` on MSVC-STL in older RemoteBackend tests, `duplicate-include`).
- `/usr/bin/clang-format` 22 `--dry-run --Werror`, `editorconfig-checker`, `check-wasm-runtime-containment.py`:
  clean.
- macOS / FreeBSD: not compiled. By reading: `SharedGuestMemory` uses only the 4.2 object API; the PduStream BSD
  branch; `HostileHost` uses `open(O_CLOEXEC)`.
- Performance: shared mode removes every MemoryRead/MemoryData/MemoryWrite; adds per Instantiate one memfd +
  ftruncate + seal + map (parent) and one CAS for the lease; per growth one round trip. Measurement is 4.7's.
- Coverage: not measured.

## Deviations

1. **Item 0.3** done by `EngineSignalHandlers`, not by `CATCH_CONFIG_NO_POSIX_SIGNALS` (cannot reach a compiled
   Catch2). See above.
2. **New fix commit** for descriptor attribution in `PduStream` (4.3 code); needed for Shared to work at all
   under concurrent release+instantiate.
3. **`request(pdu, step, grant, answer)`** takes a fourth parameter, `GrowAnswer`: the control exchange must judge
   a start function's Grow against the instance being made, which only the caller knows. The two-argument
   `request` is unchanged (a Grow there is a wrong reply).
4. **`SharedGuestMemory::refuseGrowth(bytes)`** beside `grow(bytes)`: interruption is the instance's state, not the
   memory's, and D76 judges violations before refusing.
5. **R30 cap is carried by `HostConnection::objectBytesCap`**, not by `BackendConfig`: the connector is what
   knows the data cap it starts hosts with (in-process: 1 GiB default). Tests that hold `InterruptSlotCount`
   instances (`slotExhaustionIsRuntimeUnavailable`, `releasedSlotIsLowered`, the release flood row) now use a
   one-page runtime so 1000+ live objects fit under the cap.
6. **Initial size past the cap is refused before anything in both modes**, not only Shared.
7. **The copy-path hostile rows** (`over-cap`, `bogus-size`, `stall-after-header`) now run `HostMemory::Copied`:
   with shared memory no MemoryRead exists to misbehave on.
8. **Extra row `memory-unusable`** (exit 76 at the first Grown) to pin the controller's exit-76 requirement: it
   counts as a crash of the host's own (CallCrashed, one unclean exit, no Requested kill), not a violation. The
   parent has no exit-code-specific accounting: any host death is `HostCrashed`/`HostUnavailable` to the breaker.
9. **Extra tests**: `SharedGuestMemory.objectIsClosedOnceGranted`, `Hostility.copyingHostServes`,
   `PduStream.descriptorGoesWithItsFrame`.
10. `HostServer_test`/`HostProcess_test` now ask for `makeHostedEngine(MemoryMode::Copy)` explicitly (their
    scripted parents send copy Instantiates); `RegistryRemote_test`'s death-on-read connector copies too.
11. `/simplify` not run (it dispatches agents; brief forbids). Self-review folded in: `growStatusOf` shared by
    both answer paths; one `serveHonestly` scripted host for the new backend tests.

## Concerns

1. **Scribbling barely bites.** `hostile memory through a real host`: 199 of 200 renders succeed, 1 fails with an
   extension error, in ASan and release alike. The relay scribbles only the latest grant's *initial* bytes; the
   test proves Contour survives, not that decoding is single-fetch (that is 4.6's `GuestReader` work).
2. **The release run found a tooling bug, now fixed**: the scribbler started a thread per grant after the helper
   had set `RLIMIT_NPROC = 0`, so `std::thread` threw EAGAIN and the hostile host aborted. Any future relay
   thread must start before `serve()`.
3. **Windows self-grant on failure paths**: a grant to `currentProcess()` that the in-process host never claims
   (request lost, or a Copy-mode host refusing it as a violation) leaks one duplicate. Normal cycles are proven
   leak-free under wine; the failure paths kill the incarnation but not this process.
4. **Granted-then-unopenable** still leaves the parent believing the larger size until the helper's exit-76 is
   seen; harmless to the parent (its own mapping), as 4.4 noted.
5. **BSD descriptor attribution** is by reading only; if FreeBSD/macOS group differently than assumed, the helper
   reports a violation (safe, not silent).
6. Real Windows commit semantics across views remain CI's to prove (wine commits per view).

## Fix round 1

Commit: `80b36a51b` test(vtextension): a start function's bad growth kills the host too.

**Finding:** growth during Instantiate was tested only on the happy path.

**Test added:** `RemoteBackend.badGrowDuringInstantiateKillsTheHost` (RemoteBackend_test.cpp). Five rows, each sent
through `HonestScript::beforeInstantiated` as a control-channel Grow before Instantiated: not larger (64 KiB),
unaligned (128 KiB + 1), past the cap (object + 64 KiB), another instance (id + 1), and a host whose Hello says Copy
(128 KiB). Each expects `instantiate() == unexpected(HostUnavailable)` and `terminations() == 1`. The scripted host
answers the Instantiate whenever it got a `Grown`, so a parent that answers instead of killing gets an instance
and the row fails (rather than hangs: a first version that stopped serving made mutation A wait forever; that run
was stopped and the mutated file restored from its backup).

**Mutation check** (`vtextension_remote_test "RemoteBackend.badGrowDuringInstantiateKillsTheHost"`, restored after
each, rebuilt):
- A: `grow.instanceId != instanceId` removed from the answer lambda (RemoteBackend.cpp) → the case fails, row
  "another instance" (the other rows are refused by the size and mode checks, as they should be).
- B: the Violation branch removed from `HelperIncarnation::exchange`, so a violation is answered `Grown{Refused}`
  → all five rows fail.
- Clean: 1 case, 20 assertions, passed.

**Commands and results** (clang-asan, `flock -o`, nice 19, ionice 3, -j 4, target `vtextension_remote_test`, then
the vtextension targets for ctest):
- `vtextension_remote_test` (whole): 289 cases, 12077 assertions, all passed.
- `ctest --preset=clang-asan -R vtextension`: 5/5 passed (41.5 s).
- clang-format 22 `--dry-run --Werror`, editorconfig-checker, clang-tidy on RemoteBackend_test.cpp: clean.
