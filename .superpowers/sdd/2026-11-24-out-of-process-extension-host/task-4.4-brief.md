### Task 4.4: Helper side — mapping, memory creator, growth through the parent
**Files:**
- Create:
  - `transport/GuestMemoryMapping{.hpp,_posix,_win32,_test}` (test in process_test).
  - `host/MemoryProvision{.hpp,.cpp}`.
  - `wasmtime/WasmtimeMemoryCreator{.hpp,.cpp}`.
- Modify: `wasmtime/{WasmtimeEngine.hpp,.cpp,HostedEngine.cpp,WasmtimeEngine_test.cpp}`, `testing/{HostedEngine.hpp,ProcessModules.hpp}`, `host/{HostServer.cpp,HostServer_test.cpp}`, CMake.

**Produces:**
- `MappingError { WrongSize, Io, Unsupported }`.
- `GuestMemoryMapping::map(object, objectBytes, initialBytes, guardBytes)`, `base()`, `accessibleBytes()`, `objectBytes()`, `open(bytes)` (never shrinks).
- `canMapGuestMemory()`.
- `host::MemoryProvision{OwnedHandle object; u64 objectBytes, initialBytes; function<GrowStatus(u64)> grow}`, `ProvisionScope`, `takeProvision()`.
- `wasmtime::MemorySharing { Private, Provided }`, `SignalsGuardBytes = 32 MiB`, `WasmtimeConfig::memory`.
- Test hook `testing::lastMemoryRequest() -> optional<MemoryRequest{minimum, reserved, guard}>`.
- `makeHostedEngine(MemorySharing = Private)`.
- Test modules:
  - `GrowModule`: one memory, min 1 / max 16 pages; exports `grow(pages) -> old`, `store(address, value)`.
  - `StartGrowModule`: its start function grows memory and stores the result in an exported global.

- [ ] **Step 1: Write the mapping tests.**
  - `"GuestMemoryMapping.opensOnlyTheInitialBytes"`: Linux `/proc/self/maps` shows `rw-s` / `---s` / `---p`; Windows `VirtualQuery`; other systems SKIP.
  - `"GuestMemoryMapping.openedBytesAreTheParentsBytes"`.
  - `"GuestMemoryMapping.refusesAnObjectTooSmall"`.
  - `"GuestMemoryMapping.neverShrinks"`.
  - `"GuestMemoryMapping.placeholdersWork"` (Windows; SKIP under wine if false, recorded).
- [ ] **Step 2: Write the engine tests** (Provided, Software).
  - `"WasmtimeEngine.providedMemoryBacksTheInstance"`: `store(16, 42)` is seen by the parent's mapping.
  - `"WasmtimeEngine.growthAsksAndOpens"`:
    - Granted → `grow(1) == 1`, and a store at 64 KiB + 8 is seen by the parent;
    - Refused → `grow(1) == 0xFFFFFFFF`, and `memory(65536, 1)` is `OutOfBounds`.
  - `"WasmtimeEngine.instantiateWithoutProvisionFails"`.
  - `"WasmtimeEngine.provisionMismatchFails"`.
  - `"WasmtimeEngine.reservationAndGuardAreExplicit"`: `{64 KiB, objectBytes, 0}` under Software; the 32 MiB guard under Signals runs only when not instrumented.
  - `"WasmtimeEngine.privateMemoryIsUnchanged"`.
- [ ] **Step 3: Write the server tests** (scripted parent).
  - `"HostServer.sharedInstantiateMapsTheGrant"`.
  - `"HostServer.growGoesThroughTheParent"`: Granted → `argv{1}`; Refused → `0xFFFFFFFF`.
  - `"HostServer.growOutsideALaneIsRefused"` (`StartGrowModule`: no Grow on any lane; the global is −1).
  - `"HostServer.wrongGrantIsAViolation"`, a table of six: bad `objectBytes`, `initial > object`, unaligned initial, no descriptor / zero handle, half-size object, POSIX `handle != 0`.
  - `"HostServer.anythingButGrownAnswersAGrowIsAViolation"`.
  - `"HostServer.copyEngineRefusesAGrant"`.
- [ ] **Step 4: Build** `vtextension_process_test vtextension_wasmtime_test vtextension_remote_test`; expect failure.
- [ ] **Step 5: Implement the POSIX mapping**: reserve `PROT_NONE` (`MAP_NORESERVE`), size check, `MAP_SHARED|MAP_FIXED`, `mprotect` the initial bytes.
- [ ] **Step 6: Implement the Windows mapping** with placeholders (D74); `open` checks the pages are committed.
- [ ] **Step 7: Implement the provision, the creator** (D72/D73/D75) **and the engine config.** `HelperEngine` stays Private until 4.5.
- [ ] **Step 8: Implement the server side.**
  - Copy mode: the grant must be empty and nothing attached.
  - Shared mode: check, claim, size-check, scope around instantiate.
  - `growOnCurrentLane` per D76.
- [ ] **Step 9: Run** `process_test "GuestMemoryMapping.*"`, `wasmtime_test "WasmtimeEngine.*" "[wasm-contract]"` and `remote_test "HostServer.*"` → PASS; ctest → PASS.
- [ ] **Step 10: Commit** `feat(vtextension): the extension host runs guests in memory Contour gives it`.


## Controller rulings that change this brief
- R29: growth during instantiation (a start function) is ALLOWED, not refused. While the helper's control thread runs `Instantiate`, `grow_memory(n)` sends `Grow` on the **Control** channel and reads exactly one `Grown` there (the wire already permits Grow/Grown on Control). Replace `"HostServer.growOutsideALaneIsRefused"` with `"HostServer.growDuringInstantiateGoesThroughTheParent"`: `StartGrowModule` → scripted parent sees `Grow` on control before `Instantiated`; Granted → the global holds the old page count (not −1) and the parent mapping sees the start function's store; Refused → global −1.
- R34 (Windows): `GuestMemoryMapping` commits `[old, n)` through its OWN view after `Granted` (correct whether commit is section-wide or per view). With `MapViewOfFile3` placeholders do not reuse `SharedObject_win32` `viewBytes()` for the size check — rely on the mapping failing for a too-small section (or `NtQuerySection`), and say which in the report.
- Grants are claimed as `GrantedHandle` (task 4.3) and `adopt()`ed only after a successful map; never close an unadopted Windows value.
- Small carried comments (one line each, no behaviour change): at the not-`SCM_RIGHTS` check in `Channel_posix.cpp`, note it is defence in depth (unreachable on Linux with the 2-descriptor buffer); in the 3-descriptor PduStream test, note Linux drops the third descriptor itself on truncation.
