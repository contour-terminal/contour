### Task 4.5: Parent side — an object per instance, the acknowledged size, answered growth
**Files:**
- Create: `remote/SharedGuestMemory{.hpp,.cpp,_test}` (remote_test).
- Modify:
  - `remote/{RemoteBackend.cpp,HelperIncarnation,HostRefusals,RemoteBackend_test,CopyPathMemory_test,testing/RemoteDefaultBackend,Hostility_test}`.
  - `wasmtime/{HelperEngine,HostedEngine}.cpp`, `testing/BackendContract.hpp`.
  - `contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}`.

**Produces:**
- `GrowVerdict { Violation, Refused, Granted }`.
- `SharedGuestMemory::create(cap, initial) -> expected<…, ExtensionError>` (`OutOfMemory` / `RuntimeUnavailable`), `object()`, `closeObject()`, `grant(wire)`, `view(addr, size)`, `grow(bytes)`, `acknowledgedBytes()`.
- `HelperIncarnation`: `memoryMode()`, `handleTarget()`, `request(pdu, step, grant)`.
- `HostRefusal::MemoryMode` is removed.
- The hostile relay now forwards attached descriptors, and gains these rows:

| Row | Acts when | Action |
|---|---|---|
| `grow-past-cap` | first `Grow` | bytes → objectBytes + 64 KiB |
| `grow-shrinks` | first `Grow` | bytes → 0 |
| `grow-unaligned` | first `Grow` | bytes + 1 |
| `grow-wrong-instance` | first `Grow` | id + 1 |
| `descriptor-up` (POSIX) | first `CallResult` | attach `/dev/null` |
| `scribble-memory` | every grant | map it; a thread flips bytes until EOF |
| `copy-memory` | start | inner engine Private (Hello says Copy) |

- [ ] **Step 1: Write the memory tests.**
  - `"SharedGuestMemory.boundsAreContoursSize"`.
  - `"SharedGuestMemory.growthIsChecked"`:
    - Violation: 64 KiB (not larger), 64 KiB + 1 (unaligned), 2 MiB (past the cap), 128 KiB a second time;
    - Granted: the first 128 KiB.
  - `"SharedGuestMemory.viewsSurviveGrowth"`.
  - `"SharedGuestMemory.commitFollowsAcknowledgement"` (Windows).
- [ ] **Step 2: Write the backend tests** (scripted helper).
  - `"RemoteBackend.helloMaySayEitherMode"`.
  - `"RemoteBackend.sharedMemoryNeedsNoMemoryPdus"`.
  - `"RemoteBackend.growIsAnsweredAndBoundsFollow"`.
  - `"RemoteBackend.badGrowKillsTheHost"` (table of four → `HostCrashed`, `terminate()` once).
  - `"RemoteBackend.growWhileInterruptedIsRefused"`.
  - `"RemoteBackend.initialSizeComesFromTheModule"` (2 pages → 128 KiB; over the cap → `OutOfMemory`, nothing sent).
  - `"RemoteBackend.unexportedMemoryIsOutOfBounds"`.
  - `"RemoteBackend.instancesHoldNoDescriptor"` (1000 cycles; fd count unchanged).
  - `"RemoteBackend.growDuringLoweringIsSeen"` (256 KiB string through `cabi_realloc`).
  - The copy cases pass over `makeHostedEngine(Private)`.
- [ ] **Step 3: Write the contract case** `grownMemoryIsVisible`: `grow(1) == 1`; `store(65540, 0xC0DE)`; `memory(65540, 4)` reads it. It runs on every composition.
- [ ] **Step 4: Write the hostility rows.**
  - The grow rows: `call grow` → `HostCrashed` in < 2 s, one death, `uncleanExits == 0`, lazy recovery (`launches == 2`).
  - `descriptor-up`: `add == 5`, the parent's fd count is unchanged, no death.
  - `"hostile memory through a real host"` (`scribble-memory`): 200 statusline renders within 10 s, each ok or an `ExtensionError`, `uncleanExits == 0`.
- [ ] **Step 5: Build** the remote, test and wasmtime test binaries, `contour-extension-host`, the hostile helper and `vtextension_helper_test`; expect failure.
- [ ] **Step 6: Implement `SharedGuestMemory`**: create, map RW, seal, commit the initial bytes; an atomic acknowledged size.
- [ ] **Step 7: Implement the backend.**
  - `RemoteModule` keeps the initial size and whether a memory is exported.
  - Shared-mode instantiate: create → grant → request with the grant → `closeObject`.
  - `RemoteInstance` holds the variant; the call loop answers `Grow` per D76.
  - The handshake no longer refuses a mode.
- [ ] **Step 8: Switch Shared on**: `HelperEngine` (`canMapGuestMemory()`), `HostedEngine` defaults to Provided, plus the relay forwarding and rows.
- [ ] **Step 9: Run** `remote_test` (whole), `helper_test "~[.measure]"` (whole: contract 42, `[bundled]`, Review Focus, teardown, hostility) and `vtextension_test "[wasm-contract]"` → PASS; ctest → PASS.
- [ ] **Step 10: Commit** (two):
  - `feat(vtextension): share guest memory with the extension host instead of copying it`
  - `test(vtextension): hostile growth, descriptors and memory from an extension host`


## Item 0 (carried from the previous review; do first, own commit)
1. `wasmtime/WasmtimeMemoryCreator.cpp` ~50: after a Granted grow answer, check `provision.memory.accessibleBytes() >= newBytes` and return an error otherwise (don't trust the grow request to have opened the bytes). Test with a GrowRequest double that grants without opening.
2. `host/HostServer.cpp` ~224: the `growOnCurrentChannel` doc comment is split mid-sentence by clang-format — rewrite so it formats whole.
3. Define `CATCH_CONFIG_NO_POSIX_SIGNALS` on `vtextension_wasmtime_test` (target compile definition) so a second Signals-mode case cannot have Wasmtime's handlers overwritten by Catch2; then the one-Signals-case-per-binary comment can go. Do the same for any other test target that creates a Signals engine.

## Controller requirements that change this brief
- R29 corollary (from the 4.4 review): while an `Instantiate` is pending, the parent must hold the control channel EXCLUSIVELY for the whole exchange, answering `Grow` on control with `Grown` (same checks as the lane, against the instance being created) — no `Release` from the D51 outbox, nothing else, may be interleaved, or the helper reads it as the Grow answer. Test: `"RemoteBackend.instantiateHoldsControlWhileAStartFunctionGrows"` — a release queued by another thread during a start-function grow is sent only after `Instantiated`.
- R30: sum of live `objectBytes` per incarnation ≤ `ResourceCaps::dataBytes` (default 1 GiB); an `Instantiate` that would exceed it fails `OutOfMemory` before anything is created or sent. Test `"RemoteBackend.liveObjectsStayWithinTheDataCap"`.
- Windows: a grant to `HandleTarget::currentProcess()` (in-process host compositions) leaves a handle only a claim closes — the in-process path must claim and adopt/close it; test that no handle leaks over 1000 instantiate/release cycles there (Windows-only, runs under wine).
- Exit code 76 (granted memory cannot open) exists since 4.4: Contour must treat it as a helper crash (`HostCrashed`), not a protocol violation, in the breaker accounting.
