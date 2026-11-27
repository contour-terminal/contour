# Task 4.4 report: helper side — mapping, memory creator, growth through the parent

Status: **DONE**, with the deviations below. Branch `feature/out-of-process-extension-host`, base `4b7ebf90d`. Nothing pushed.

## Commits

| Hash | Subject |
|---|---|
| `faae235f2` | docs(vtextension): note which descriptor checks Linux makes before ours (the two carried comments) |
| `c2bd2249e` | feat(vtextension): the extension host runs guests in memory Contour gives it |

Both signed off, no AI trailers.

## What changed

New:
- `remote/transport/GuestMemoryMapping{.hpp,.cpp,_posix.cpp,_win32.cpp,_test.cpp}` (in `vtextension_remote_wire`; test in `vtextension_process_test`).
  - `MappingError { WrongSize, Io, Unsupported }`; `GuestMemoryMapping::map(object, objectBytes, initialBytes, guardBytes)`, `base()`, `accessibleBytes()`, `objectBytes()`, `guardBytes()`, `open(bytes)` (never shrinks); `isGuestMemoryLayout()`; `canMapGuestMemory()`.
  - Every size must be whole 64 KiB pages (also Windows' allocation granularity, so placeholders split there).
  - POSIX: `checkSharedObjectSize` → reserve object+guard `PROT_NONE|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE` → `mmap(MAP_SHARED|MAP_FIXED, PROT_READ|PROT_WRITE)` over the start (proves write access) → `mprotect(PROT_NONE)` of `[initial, object)`. `open` = `mprotect` RW. One `munmap` releases all.
  - Windows (D74/R34): `VirtualAlloc2(MEM_RESERVE|MEM_RESERVE_PLACEHOLDER)` for object+guard, split with `VirtualFree(MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)`, `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE)` over the object's placeholder, then `VirtualAlloc(MEM_COMMIT)` of `[0, initial)` **through its own view**; `open(n)` commits `[old, n)` the same way. Functions resolved from kernelbase by `GetProcAddress`. `canMapGuestMemory()` = functions present + a 64 KiB probe that maps, opens and writes; cached.
  - **R34 size check:** I did not reuse `viewBytes()` and did not use `NtQuerySection`. The check is the mapping itself: `MapViewOfFile3` of `objectBytes` fails for a smaller section (→ `Io`). `NtQuerySection` would need `SECTION_QUERY`, which the 4.3 grant (`FILE_MAP_READ|FILE_MAP_WRITE`) does not include. A *larger* section is not detected on Windows; only `objectBytes` of it is ever mapped, so this is harmless.
- `host/MemoryProvision{.hpp,.cpp}` (engine-neutral, in `vtextension_host`): `GrowRequest = function<GrowStatus(u64)>`, `MemoryProvision { GuestMemoryMapping memory; GrowRequest grow; }`, RAII `ProvisionScope` (thread-local, nests), `takeProvision()`.
- `wasmtime/WasmtimeMemoryCreator{.hpp,.cpp}`: `provideMemories(wasm_config_t*)` installs the creator; `testing::MemoryRequest{minimum, reserved, guard}` and `testing::lastMemoryRequest()` (thread-local). The creator takes the thread's provision once, requires `minimum == accessibleBytes`, `reserved == objectBytes`, `guard <= guardBytes` (else a Wasmtime error → `InvalidComponent`). `grow_memory(n)`: `n <= open` → ok; `n > object` → error; `grow(n) != Granted` → error (→ `memory.grow` = -1); `Granted` → `open(n)`. The finalizer unmaps.

Modified:
- `wasmtime/WasmtimeEngine.{hpp,cpp}`: `MemorySharing { Private, Provided }`, `SignalsGuardBytes = 32 MiB`, `WasmtimeConfig::memory`. Provided sets `memory_reservation = memoryObjectBytes(limits.memoryBytes)`, `memory_guard_size` (32 MiB Signals / 0 Software), `memory_reservation_for_growth = 0`, `memory_may_move = false`, `memory_init_cow = false`, and the creator. `memoryMode()` is `Shared` for Provided. `HelperEngine` untouched: still Private/Copy.
- `host/IHostedEngine.hpp`: new `memoryGuardBytes()`; `testing/DecoratedEngine.hpp` forwards it.
- `testing/HostedEngine.hpp` + `wasmtime/HostedEngine.cpp`: `makeHostedEngine(proto::MemoryMode = Copy)`.
- `testing/ProcessModules.hpp`: `GrowModule`, `StartGrowModule` (+ `StartGrowAddress`, `StartGrowValue`), hand-encoded, WAT in the comments.
- `host/HostServer.{hpp,cpp}`:
  - `RuntimeSlot::memoryBytes`; `provisionOf()` — Copy: `memory == MemoryGrant{}` and nothing attached; Shared: `objectBytes == memoryObjectBytes(cap)`, `initial <= object`, `initial % 64 KiB == 0`, `claimGrantedHandle`, `GuestMemoryMapping::map(..., engine.memoryGuardBytes())`, then `adopt()` and close. Every failure is `ProtocolViolation`.
  - Instantiate runs inside a `ProvisionScope` with `CurrentInstantiation = this`.
  - `growOnCurrentChannel()` (D76 + R29): `CurrentLane` → that lane; else `CurrentInstantiation == this` → control; else Refused. `askToGrow()` sends `Grow` and reads exactly one PDU; anything but `Grown`, or a channel failure, records the exit (lane's `ended`, or `_instantiationEnded`) and returns Refused; the request then ends the connection once the guest returns.
  - Any non-Instantiate control PDU that brought a descriptor is a violation.
- CMake: the new sources; `vtextension_wasmtime` links `vtextension_host` publicly (for `MemoryProvision`).
- Carried comments: `Channel_posix.cpp` (not-`SCM_RIGHTS` check is defence in depth) and `PduStream_test.cpp` (Linux drops the third descriptor on truncation).

## Wasmtime API facts relied on (Wasmtime 48.0.10, `out/clang-asan/_deps/wasmtime-src/include/wasmtime/config.h`)

Signatures: `wasmtime_new_memory_callback_t(void* env, const wasm_memorytype_t*, size_t minimum, size_t maximum, size_t reserved_size_in_bytes, size_t guard_size_in_bytes, wasmtime_linear_memory_t* out)`; `wasmtime_memory_get_callback_t(void* env, size_t* byte_size, size_t* byte_capacity) -> uint8_t*`; `wasmtime_memory_grow_callback_t(void* env, size_t new_size) -> wasmtime_error_t*`; `wasmtime_config_host_memory_creator_set` copies the struct. Measured by a temporary probe print (reverted), GrowModule, cap 4 MiB:

| | Software | Signals |
|---|---|---|
| `minimum` | 65536 | 65536 |
| `maximum` | 1048576 (module's declared max, bytes) | 1048576 |
| `reserved_size_in_bytes` | 4194304 (= `memory_reservation`) | 4194304 |
| `guard_size_in_bytes` | 0 | 33554432 |

- `grow_memory` receives the **new total byte length** (131072 for `memory.grow 1` from one page).
- An error from `grow_memory` makes `memory.grow` return -1, not a trap (with the backend's store limiter).
- A creator error fails `wasmtime_linker_instantiate` (→ `InvalidComponent`).
- The creator is called on the instantiating thread (the thread-local provision is found).
- The Signals run was under ASan (instantiation only, no fault); the committed Signals section stays skipped in instrumented builds.

## Tests, and how each was seen failing

Written together with the implementation. Failure shown by mutation (two rounds, all files restored and checked with `cmp`):

| Test | Mutation → result |
|---|---|
| `GuestMemoryMapping.opensOnlyTheInitialBytes` | tail not `mprotect`ed closed → fails; `MAP_PRIVATE` → fails (`rw-p`) |
| `GuestMemoryMapping.openedBytesAreTheParentsBytes` | `MAP_PRIVATE` → fails |
| `GuestMemoryMapping.refusesAnObjectTooSmall` | size check dropped → fails |
| `GuestMemoryMapping.neverShrinks` | `open` treats only `==` as no-op → fails |
| `GuestMemoryMapping.layoutIsWholeGuestPages` (extra) | not mutated |
| `GuestMemoryMapping.placeholdersWork` (Windows) | passes under wine (no SKIP: wine has placeholders) |
| `WasmtimeEngine.providedMemoryBacksTheInstance` | `MAP_PRIVATE` → fails |
| `WasmtimeEngine.growthAsksAndOpens` | growth ignores the answer → "refused" fails (1 ≠ 0xFFFFFFFF); `MAP_PRIVATE` → "granted" fails |
| `WasmtimeEngine.instantiateWithoutProvisionFails` | creator installed on Private engines too → (with privateMemoryIsUnchanged) every Copy-mode instantiation fails; not mutated alone |
| `WasmtimeEngine.provisionMismatchFails` | minimum check dropped → fails |
| `WasmtimeEngine.reservationAndGuardAreExplicit` | `memory_guard_size` not set → fails (Software section) |
| `WasmtimeEngine.privateMemoryIsUnchanged` | creator installed on Private engines → fails |
| `HostServer.sharedInstantiateMapsTheGrant` | `MAP_PRIVATE` → fails |
| `HostServer.growGoesThroughTheParent` | growth ignores the answer → refused fails; `MAP_PRIVATE` → granted fails |
| `HostServer.growDuringInstantiateGoesThroughTheParent` (R29) | `CurrentInstantiation` not set → fails; growth ignores answer → fails |
| `HostServer.wrongGrantIsAViolation` (6 rows) | cap check dropped → fails; size check dropped → fails (half-size row) |
| `HostServer.anythingButGrownAnswersAGrowIsAViolation` (lane ×2, control) | non-`Grown` answer not recorded as a violation → fails |
| `HostServer.copyEngineRefusesAGrant` | Copy check dropped → fails |
| `HostServer.onlyAnInstantiateBringsAHandle` (extra, POSIX) | attachment check dropped → fails |

The six-row table uses, on Windows, "a value naming no object" (0x1F4) in place of the POSIX row "handle ≠ 0 beside the descriptor".

## Results

- Linux (clang-asan):
  - `process_test "GuestMemoryMapping.*"`: 5 cases, 47 assertions, passed.
  - `wasmtime_test "WasmtimeEngine.*"`: 15 cases (14 passed, 1 skipped = the Signals section in an instrumented build), 195 assertions.
  - `wasmtime_test "[wasm-contract]"`: 41 cases, 482 assertions.
  - `remote_test "HostServer.*"`: 37 cases, 1561 assertions.
- **`ctest --preset=clang-asan -R vtextension`: 5/5 passed** (40.6 s), on the final tree before committing.
- clang-tidy (`-p out/clang-asan`) on every changed `.cpp`: clean apart from the known `pro-type-member-init` noise. I fixed three `readability-use-concise-preprocessor-directives` findings in the test. Windows (`-p out/clangcl-debug`) on `GuestMemoryMapping_win32.cpp`, its test and `HostServer.cpp`: clean apart from that noise and `duplicate-include`.
- `/usr/bin/clang-format` (22.1.8) `--dry-run --Werror`, `editorconfig-checker` and `scripts/check-wasm-runtime-containment.py`: clean.
- **Windows** (`out/clangcl-debug`, clang-cl, `-Werror`): `vtextension_process_test vtextension_remote_test contour-extension-host vtextension_helper_test extension_host_probe` build clean.
- **wine 11.20** (own prefix `scratchpad/wine44`, `WINEPATH` = mingw `zlib1.dll`), final tree:
  - `process_test.exe "GuestMemoryMapping.*"`: 6 cases, 53 assertions, all passed (**`canMapGuestMemory()` is true under wine; nothing skipped**).
  - full `process_test.exe`: 98 cases (97 passed, 1 skipped = the existing cross-view case), 721 assertions.
  - `remote_test.exe "HostServer.*"`: 36 cases, 1519 assertions, all passed (shared-memory server tests included).
  - `remote_test.exe "HostProcess.*,HostBootstrap.*,RemoteBackend.*"` (one tree earlier, before a test-only change): 49 cases, 6741 assertions, all passed.
  - `helper_test.exe "~[.measure]"`: 124 cases (122 passed, 2 skipped), 2882 assertions.
- macOS and FreeBSD were not compiled. By reading: the POSIX mapping uses only `mmap`, `mprotect`, `munmap` and `MAP_NORESERVE` (defined on both) and the existing `checkSharedObjectSize`.
- Performance: Copy mode is unchanged apart from one `takeAttachedHandle()` per control PDU, a pointer check. Shared mode adds one map per Instantiate and one round trip per growth.

## Deviations

1. **`MemoryProvision` holds the mapping, not `{OwnedHandle object; objectBytes; initialBytes}`. The server maps; the creator does not.** Three reasons:
   - The ruling says adopt only after a successful map.
   - A half-size object must be a *violation*, not an instantiation error.
   - A module without memory never reaches the creator. On Windows an unproven handle can only be forgotten, so it would leak.

   Mapping before instantiation needs the guard, so `IHostedEngine::memoryGuardBytes()` was added. The creator checks Wasmtime's guard against the mapping's.
2. **`makeHostedEngine(proto::MemoryMode)`, not `MemorySharing`:** the containment lint forbids `testing/` to include a backend header.
3. **`HostServer.growOutsideALaneIsRefused` is replaced by `growDuringInstantiateGoesThroughTheParent`**, per R29. `StartGrowModule` exports a getter function `grown` rather than the global itself, because the parent can only Call functions.
4. **Extra tests:** `GuestMemoryMapping.layoutIsWholeGuestPages`, `HostServer.onlyAnInstantiateBringsAHandle`, and a control-channel section in `anythingButGrownAnswersAGrowIsAViolation`.
5. **The Windows unmap uses plain `UnmapViewOfFile`** (it frees the placeholder's range) and `VirtualFree` for the guard placeholder. `UnmapViewOfFile2` is not resolved.
6. **`GuestMemoryMapping.cpp`** (shared moves and `open`) is added beside `_posix` and `_win32`, following `SharedObject.cpp`.
7. **`/simplify` was not run:** it dispatches agents, which this brief forbids. I did a self-review in its place. It made the attachment test POSIX-only, so that a Windows test leaks no duplicate, and tidied comments.

## Concerns

1. **A granted growth that the helper then fails to open** (`mprotect` or `VirtualAlloc` refused) makes `memory.grow` return -1 while the parent has already acknowledged `n`. This is harmless to the parent, which reads its own mapping, but the two sides disagree. Task 4.5 could treat it as Refused only if the helper told the parent, which the protocol has no message for.
2. **The Signals path was not run in a committed test.** The section skips in the asan tree, and the brief limits me to that tree. Its values were confirmed only by the temporary probe above. The release tree or CI has to run it.
3. **Windows commit semantics:** wine passes, but wine commits per view. The helper commits through its own view (R34), which is correct either way. Real-Windows behaviour (and the 4.3 cross-view test) is still CI's to prove.
4. **macOS/FreeBSD are uncompiled.**
5. **Windows grants refused before the claim** (bad sizes, Copy mode with a value) leave a duplicated handle in the helper unclosed, by the release-not-close rule. The helper dies on the violation anyway.
6. **`maximum` is not checked by the creator.** Growth is bounded by the object and by the parent's answer, so this is safe; it is noted for 4.5's cap logic.

## Fix round 1

Commits on top of `c2bd2249e`:

| Hash | Subject | Findings |
|---|---|---|
| `3780e3062` | fix(vtextension): reserve guest memory without MAP_NORESERVE | 1 |
| `b4fd8271f` | build(vtextension): give the memory provision a target of its own | 4 |
| `4d1b596be` | fix(vtextension): end the extension host when granted memory cannot open | 2, 5, 6 |
| `891522849` | test(vtextension): trap through signals in memory Contour provides | 3 |

1. **`MAP_NORESERVE` is dropped, not guarded.** The reservation is private and `PROT_NONE`. Linux charges only writable private mappings against the commit limit, and the guard never becomes writable, so the flag bought nothing anywhere. FreeBSD 11 and later do not define it. Dropping it leaves one code path. The reasoning is in the file comment.
2. **Local failure.** The fix changes the grow request rather than adding a report hook.
   - `GrowRequest` is now `GrowStatus(GuestMemoryMapping& memory, uint64_t bytes)`: it asks Contour and, once Contour grants, opens the bytes itself. The creator only calls it.
   - In the server, `growOnCurrentChannel()` asks on the lane or on control, then opens through `HostServerConfig::openGuestMemory`. That is a new DI seam, defaulting to `&GuestMemoryMapping::open`.
   - If the open fails, the server sets `ServerExit::MemoryUnusable` on the channel the growth was asked on (the lane's `ended`, or `_instantiationEnded`). The connection ends once the engine returns, and no `CallResult` or `Instantiated` is sent.
   - New rows: `HostProcessExit::MemoryUnusable`, code **76**, "memory Contour granted an extension could not be opened", and `hostProcessExitOf` maps it. The exit-table tests were updated.
   - The guest itself still sees -1 from `memory.grow`. Wasmtime's C API gives a grow callback no way to trap.
   - Test `HostServer.grantedGrowthThatCannotBeOpenedEndsTheHost`, with a lane section and a start-function section, uses an injected opener that fails. Seen failing: with the `MemoryUnusable` assignment removed, both sections fail (exit was not `MemoryUnusable`, and the call was answered).
3. **The Signals path, run in `out/clang-release`** after the ASan build, under the same lock and nice, building `vtextension_wasmtime_test` only.
   - `WasmtimeEngine.signalTrapsOnProvidedMemory`: Provided memory with a 32 MiB guard. It checks `lastMemoryRequest() == {initial, 4 MiB, 32 MiB}`, then makes two stores. Each traps (`CallFailure::Trap`), the instance still stores afterwards, and the process survives:
     - one past the open bytes, inside the object;
     - one past the object, with offset 8 from 4 MiB − 4.
   - **No abort, and nothing to fix.** strace shows Wasmtime 48 bounds by the reservation (the object), not by the current size:
     - the first store faults `SEGV_ACCERR` in the closed pages, which Wasmtime treats as a trap;
     - the second fails the explicit check and is sent to address 0 (Spectre hardening), giving `SEGV_MAPERR` at NULL, also a trap.
   - So the 32 MiB guard is never reached with a 4 MiB reservation, and the `wasm_accessible()` concern does not arise. The capacity reported stays `objectBytes`.
   - Consequence: the closed tail (POSIX `PROT_NONE`, Windows uncommitted pages) is the bounds enforcement under signal traps, not a second line of defence. This is recorded in the plan's facts.
   - **Harness fact.** Catch2 restores its own signal handlers after every case, over the handlers Wasmtime installs once per process. A second case that brought up a Signals engine therefore crashed the full release run (SIGSEGV fatal in Catch). I moved the Signals section of `reservationAndGuardAreExplicit` into the new case, which is now the binary's only Signals case, and noted the rule in the test.
4. **Dependency.** `vtextension_memory_provision` (MemoryProvision.cpp/.hpp; links `vtextension_remote_wire`) is defined in `host/CMakeLists.txt`.
   - `vtextension_host` links it publicly.
   - `vtextension_wasmtime` links it privately, instead of `vtextension_host` publicly.
   - `vtextension_wasmtime_test` links it directly, since it uses the provision and transport headers.
5. `CurrentInstantiation` is set and restored by a `core::Finally` guard.
6. The `memoryGuardBytes()` comment is rewritten within 110 columns, so clang-format leaves it whole.

### Results
- ASan:
  - `WasmtimeEngine.*`: 16 cases, 195 assertions (the Signals case skips).
  - `HostServer.*`: 38 cases, 1615 assertions.
  - `HostServer.*,HostProcess.*`: 49 cases, 1851 assertions (one tree earlier).
  - `GuestMemoryMapping.*`: 5 cases, 47 assertions.
  - **`ctest --preset=clang-asan -R vtextension`: 5/5 passed.**
- Release (`out/clang-release`, `vtextension_wasmtime_test` only): the whole binary, 61 cases and 1146 assertions, passed; `WasmtimeEngine.*`: 16 cases, 226 assertions.
- clang-tidy on every changed `.cpp`: clean after one fix (`bugprone-use-after-move` across the two test sections, now a config factory). clang-format 22, editorconfig-checker and the containment lint: clean.
- Windows cross-build (the five targets): clean. One wine run, `vtextension_remote_test.exe "HostServer.*,HostProcess.*"`: 48 cases, 1807 assertions, all passed.
- The commits were not built one at a time. The final tree was built and tested.

### Remaining concerns
- The guest still sees -1 on a failed open, because Wasmtime gives a grow callback no way to trap. The connection ends once it returns. A guest that loops after a -1 keeps its lane busy until the parent's deadline or an interrupt; the same is true of the protocol-violation path.
- The guard's size is never exercised by Wasmtime 48 at these reservations. It stays as configured (D73) in case a later Wasmtime elides checks.
