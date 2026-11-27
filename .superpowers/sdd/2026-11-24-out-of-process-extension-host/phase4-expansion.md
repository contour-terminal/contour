## Phase-4 decisions

Proposed for a controller ruling. Each names the choice and, after the dash, why.

- D62. Seven tasks: 4.1 carried minors → 4.2 shared objects → 4.3 handle passing + wire v3 → 4.4 helper side → 4.5 parent side (Shared switched on) → 4.6 single-fetch decoding → 4.7 gate. — Growth spans both processes, and each side tests alone against a scripted peer. The minors touch files 4.3 edits. 4.6 needs only 4.2.
- D63. One switch, `CONTOUR_BUILD_INSTRUMENTATION` (0 none / 1 coverage / 2 sanitizers). It is a PUBLIC definition on `vtextension_remote_wire`, read only by `wasm/BuildInstrumentation.hpp` (`enum class BuildInstrumentation : uint8_t { None, Coverage, Sanitizers }`, `ThisBuildInstrumentation`). It replaces `CONTOUR_SANITIZED_BUILD`, `CONTOUR_INSTRUMENTED_BUILD` and `CONTOUR_HELPER_SOFTWARE_TRAPS`; caps, helper stderr and trap mode become constexpr reads. — Three macros with three conditions become one value.
- D64. M2: `LinuxProcessLauncher(PidfdWaits)`; `makeSystemProcessLauncher()` probes once and injects; `makeLinuxProcessLauncher(PidfdWaits)` goes in a Linux-only `process/Process_linux.hpp`. — DI without widening the cross-platform API.
- D65. M3: one `launchErrors()` table in `ProcessLauncher.hpp`, rows `{error, text, refusal}`. `launchErrorText`, the connector's refusal mapping and `refusalLine()` read it; the launch log lines become `"The extension host cannot be started: {text}. Extensions do not run."`. — Each failure is phrased once.
- D66. `hardenProcess()` returns `std::expected<void, HardeningStep>` over per-OS rows `{step, text, apply}` (`CoreDumps`, `Dumpable`, `Tracing`, `FaultReports`). Every row runs, and the first failure is returned. `beforeThreads` takes that type. The helper tells `hardening failed: <text>` once, and Hello is still `Failed`. — The log names the step, and hardening stays complete.
- D67. One creation module, `transport/SharedObject{.hpp,_posix,_win32}`, for the control page and guest memory (as D46 foresaw). `fitsSharedObject(object, expected)` accepts sizes from `expected` up to `roundUp(expected, MaxPageBytes = 16 KiB)`, whatever the reader's own page size. — Fixes M1: an x86_64 helper under Rosetta sees a 4 KiB page while the arm64 parent's object was rounded to 16 KiB.
- D68. One object per instance, always, even for modules without memory.
  - Size: `proto::memoryObjectBytes(cap) = max(roundUp(cap, 64 KiB), 64 KiB)`.
  - Linux/FreeBSD ≥ 13: memfd sealed `SHRINK|GROW`, then `SEAL` once mapped. GROW goes beyond the spec's `SHRINK|SEAL`; it is strictly stronger.
  - macOS: `/contour-eh-<pid>-<serial>`, `O_EXCL`, unlinked at once, `ftruncate` once.
  - Windows: a `SEC_RESERVE` section.
  - The parent maps the whole object RW once (it never moves) and closes its handle once the `Instantiate` carrying it is sent. — No descriptor per instance (RLIMIT_NOFILE with many panes).
- D69. Handle passing rides the **control channel with `Instantiate`** (already there per D9/D11).
  - POSIX: `SCM_RIGHTS` on the frame's first byte. Only the helper's control stream (Control, arriving ToHelper) uses `recvmsg` (`MSG_CMSG_CLOEXEC`; macOS `fcntl`) and holds at most one unclaimed descriptor. A second one, `MSG_CTRUNC` or a non-`SCM_RIGHTS` cmsg → `Malformed`.
  - Windows: the parent `DuplicateHandle`s the section (`FILE_MAP_READ|FILE_MAP_WRITE`) into the helper. It uses a `PROCESS_DUP_HANDLE` duplicate that the launcher hands out (`IProcessHandle::handleTarget()` → `SupervisedHelper` → `IHostHandle::handleTarget()`). The value travels in `Instantiate.memory.handle`, which must be 0 on POSIX.
  - **Lost:** a failed send kills the incarnation, and the handle dies with the helper.
  - **Forged:** the helper refuses what it cannot map as an object of `objectBytes` with write access, as a protocol violation. A bogus Windows value is released, not closed.
- D70. Nothing flows up. Parent reads stay plain `recv()`, so the kernel discards descriptors a helper attaches; on Windows the parent never reads a handle value. — Pinned by a test and a hostile row.
- D71. Wire v3:
  - `Instantiate.memory = MemoryGrant{u64 handle, objectBytes, initialBytes}`; `objectBytes == 0` means a copy-mode host. Row cap 32 → 64.
  - `Grow{u32 instanceId, u64 bytes}`: Lane, ToParent, cap 16.
  - `Grown{GrowStatus}`: Lane, ToHelper, cap 8; `GrowStatus { Refused, Granted }`. Kinds 16 and 17.
  - The `Memory*` PDUs stay for Copy; `DecodeError` is unchanged.
- D72. The initial size comes from the parent's own parse (`binary::largestInitialMemoryPages(_bytes)`). Above the cap → `OutOfMemory` before anything is sent. The helper's creator requires Wasmtime's `minimum == initialBytes` and `reserved == objectBytes`.
- D73. Wasmtime config in Provided mode:
  - `memory_reservation = objectBytes`.
  - `memory_guard_size` = 32 MiB under Signals, 0 under Software.
  - `reservation_for_growth = 0`, `may_move = false`, `init_cow = false`.
  - `wasmtime_config_host_memory_creator_set`.
  - `WasmtimeConfig` gains `MemorySharing { Private, Provided }`.
  - The helper mapping (`transport/GuestMemoryMapping`, std-only so it runs under wine) reserves object + guard inaccessible, maps the object `MAP_SHARED|MAP_FIXED` at the base and opens only `[0, initial)`.
- D74. Windows maps through placeholders (`VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2`), resolved from kernelbase by `GetProcAddress`, plus a 64 KiB probe (`canMapGuestMemory()`). If unavailable, the helper's Hello says `Copy` and the copy path serves. — Stated in the handshake, equally safe.
- D75. **Deviation in wording:** the provision reaches the engine-wide creator through an engine-neutral thread-local (`host::MemoryProvision`, RAII `ProvisionScope`). HostServer's **control thread** sets it, not "the executor" (spec §5.4), because instantiation runs on control (D11). The creator takes it once; none, or a second memory → `InvalidComponent`.
- D76. Growth.
  - Helper: `grow_memory(n)` sends `Grow` on `CurrentLane` and reads exactly one PDU, which must be `Grown`. Granted → `open(n)`; Refused → `memory.grow` returns −1.
  - **Outside a lane call (a start function) growth is refused**, as host calls are there (D11). This differs from in-process engines.
  - Parent, in every call loop: a violation unless the id is the innermost frame's, `n % 64 KiB == 0` and `acknowledged < n ≤ min(cap, objectBytes)`.
  - Parent answers: interrupted → Refused; Windows commit failure → Refused; otherwise commit, store `n` (release) and answer Granted.
- D77. `memory()` is bounded by the parent's acknowledged size (acquire), never by anything the helper reports. A module that exports no memory → `OutOfBounds`. Views stay valid across growth.
- D78. The mode follows each incarnation's Hello. Both modes are accepted: `HostRefusal::MemoryMode` is removed, and `RemoteInstance` holds `variant<CopyPathMemory, SharedGuestMemory>`.
- D79. Single fetch: `abi::IGuestMemory::read(Address, span<byte> into)` (one memcpy) becomes the only way decoders read guest bytes. `view()` is renamed `writable()` (lowering and range proofs only). — Enforced by `"GuestReader.fetchesEachByteOnce"`, whose `writable()` hands decoders a decoy buffer.
- D80. Hostile memory is tested two ways, with no new executable: `extension_host_probe --mutate` for the abi-level case, and the hostile relay's `scribble-memory` row end to end.
- D81. **Residual risk, needs a ruling.** Guest memory leaves D38's RLIMIT_DATA (Linux does not count `MAP_SHARED`). A compromised helper can dirty whole objects of all live instances (≤ 1024 × 32 MiB), keep released ones mapped until it dies, and on Windows commit section pages through its own view. Proposed: accept and document. The alternative is a per-incarnation byte budget.
- D82. Compositions: remote_test's in-process host runs Shared, plus a Copy composition (`makeHostedEngine(Private)`); wasmtime_test stays Private; the real helper runs Shared. The contract gains `grownMemoryIsVisible` (42 cases).
- D83. Gate: the phase closes only if the real helper's steady plasma median is below 18.1 µs (phase 3's best). Overhead is reported as (helper − in-process Wasmtime) / helper, against the copy path in the same build via the hostile row `copy-memory`.
- D84. Phase 5 inputs:
  - seccomp: `recvmsg` on control, `mmap(MAP_SHARED|MAP_FIXED)` and `mprotect` of received descriptors, `close`.
  - Seatbelt: map received shm (`ipc-posix-shm-*` by the `/contour-eh-` prefix).
  - AppContainer: map a duplicated-in section.
  - Landlock is unaffected.

Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <t>`. Tests start with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`. Binaries are under `out/clang-asan/src/vtextension/` (`vtextension_test`), `.../wasm/remote/` (process, remote and helper tests) and `.../wasm/wasmtime/`. Before each commit: `ctest --preset=clang-asan -R vtextension`, clang-format, editorconfig-checker. After each task: `/simplify`.

## Phase 4 — Shared guest memory

### Task 4.1: Carried minors
**Files:**
- Create: `wasm/BuildInstrumentation.hpp`, `remote/process/Process_linux.hpp`.
- Modify: `process/{Process_linux,ProcessLauncher.hpp,ProcessHostConnector,PosixLaunch,Process_win32,ExtensionHostSupervisor,ProcessLauncher_test}`, `remote/HostRefusals{.hpp,.cpp,_test}`, `host/{HostBootstrap.hpp,_posix,_win32,HostProcess.hpp,.cpp,_test}`, `wasmtime/HelperEngine.cpp`, three CMakeLists.

**Produces:**
- `BuildInstrumentation` and `ThisBuildInstrumentation`.
- `LaunchErrorRow{LaunchError error; string_view text; optional<HostRefusal> refusal}`, `launchErrors()`.
- `refusalLine(HostRefusal) -> std::string`.
- `PidfdWaits`, `probePidfdWaits()`, `makeLinuxProcessLauncher(PidfdWaits)`.
- `HardeningStep`, `HardeningRow{step, text, bool(*apply)() noexcept}`, `hardeningSteps()`.
- `hardenProcess(span<HardeningRow const>) -> expected<void, HardeningStep>`, `hardenProcess()`, `hardeningStepText()`.
- `BuildCapPolicy` derived from `ThisBuildInstrumentation`.

- [ ] **Step 1: Write the tests.**
  - `"ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch"` (Linux): `→ LaunchError::Unsupported`; with the helper ends dropped, the parent control end `receiveSome` → `Closed` (no child).
  - `"ProcessLauncher.thisKernelWaitsOnPidfds"`: `== Supported`.
  - `"LaunchErrors.oneRowPerError"`.
  - `"HostRefusals.launchLinesSayWhatTheLauncherSays"`: `refusalLine(r) == format(prefix, row.text)` and `launchErrorText(e) == row.text`; `Stopped` has no refusal.
  - `"HostRefusals.everyRefusalHasALine"`: none names wasm/WAMR/Wasmtime.
  - `"HostBootstrap.hardeningNamesTheFirstFailedStep"`: fake rows 2 and 3 fail → `error() == rows[1].step`, all three ran.
  - `"HostProcess.failedHardeningSaysWhichStep"`: Hello `Failed`; the step's text is non-empty.
  - `"BuildInstrumentation.drivesCapsAndStderr"`: `BuildCapPolicy` and `defaultHelperStandardError()` match the instrumentation; asan tree → `Sanitizers`.
- [ ] **Step 2: Build** `vtextension_process_test vtextension_remote_test`; expect compile failures.
- [ ] **Step 3: Implement M2** (D64).
- [ ] **Step 4: Implement M3** (D65).
- [ ] **Step 5: Implement hardening rows** (D66).
- [ ] **Step 6: Implement the instrumentation switch** (D63). Add `CONTOUR_BUILD_INSTRUMENTATION=$<IF:$<BOOL:${CONTOUR_SANITIZE}>,2,$<IF:$<BOOL:${CONTOUR_COVERAGE}>,1,0>>` on `vtextension_remote_wire` and delete the old three definitions.
- [ ] **Step 7: Run** `vtextension_process_test "ProcessLauncher.*" "LaunchErrors.*" "HostRefusals.*" "BuildInstrumentation.*"` and `vtextension_remote_test "HostBootstrap.*" "HostProcess.*"` → PASS. Grep for the three old macros → none; ctest → PASS.
- [ ] **Step 8: Commit** (four commits):
  - `refactor(vtextension): inject whether the kernel waits on pidfds into the launcher`
  - `refactor(vtextension): phrase each launch failure once`
  - `feat(vtextension): say which hardening step of the extension host failed`
  - `refactor(vtextension): one switch says how a build is instrumented` (CMake and code are inseparable here).

### Task 4.2: Shared objects, one creation path
**Files:**
- Create: `transport/{OwnedHandle.hpp,SharedObject.hpp,SharedObject_posix.cpp,SharedObject_win32.cpp,SharedObject_test.cpp}`. The test goes in `vtextension_process_test` (it runs under wine).
- Modify: `transport/ControlPage{.hpp,_posix,_win32,_test}`, `Channel_{posix,win32}` (`closeNativeHandle`), CMake.

**Produces:**
- `OwnedHandle` (`get`, `release`, `operator bool`).
- `MaxPageBytes = 16 KiB`, `constexpr fitsSharedObject(object, expected)`.
- `SharedObjectUse { GuestMemory, InterruptPage }`, `MapAccess`.
- `createSharedObject(bytes, use) -> expected<OwnedHandle, SharedObjectError>`.
- `sealSharedObject(handle, use)`.
- `checkSharedObjectSize(handle, expected)` (POSIX).
- `SharedMapping::map(handle, bytes, access)`, `bytes()`, `commit(from, to)` (Windows `VirtualAlloc MEM_COMMIT` through the view; no-op elsewhere).
- `isControlPageSize` is removed.

- [ ] **Step 1: Write the tests.**
  - `"SharedObject.sizeMayBeRoundedUpToTheLargestPage"`:
    - accepted: `fits(16384, 4096)` (the Rosetta case), `fits(32 MiB, 32 MiB)`;
    - refused: `fits(16385, 4096)`, `fits(4095, 4096)`, `fits(32 MiB + 16 KiB, 32 MiB)`.
  - `"SharedObject.writesAreSeenThroughASecondMapping"`.
  - `"SharedObject.isSealedAgainstResizing"` (Linux/FreeBSD): `ftruncate` → EPERM; seals are `SHRINK|GROW|SEAL`, no `FUTURE_WRITE` for `GuestMemory`.
  - `"SharedObject.isNotInherited"`.
  - `"SharedObject.checkRefusesAWrongSize"`.
  - `"SharedObject.reservedSectionCommitsOnlyWhatIsAsked"` (Windows `VirtualQuery`).
  - Delete `ControlPage.sizeMayBeRoundedUpToThePage`; the other ControlPage cases are unchanged.
- [ ] **Step 2: Build** `vtextension_process_test`; expect failure.
- [ ] **Step 3: Implement POSIX** (creation moves out of `ControlPage_posix`).
- [ ] **Step 4: Implement Windows** (`SEC_RESERVE` for `GuestMemory`; `VirtualQuery` size check on map).
- [ ] **Step 5: Rebuild ControlPage on SharedObject**; POSIX adopt uses `checkSharedObjectSize` (fixes M1).
- [ ] **Step 6: Run** `vtextension_process_test "SharedObject.*" "ControlPage.*"` → PASS; ctest → PASS.
- [ ] **Step 7: Commit** (two):
  - `fix(vtextension): accept a shared object rounded up to any page a parent may use`
  - `feat(vtextension): reserved shared objects whose pages Contour commits`

### Task 4.3: Handing an object to the helper; protocol v3
**Files:**
- Create: `transport/HandleGrant{.hpp,_posix,_win32}`.
- Modify:
  - `transport/{Channel,PduStream}` and their tests.
  - `proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`.
  - `process/{ProcessLauncher.hpp,Process_*.cpp,ExtensionHostSupervisor,ProcessHostConnector,testing/FakeProcessLauncher,testing/LaunchProbe.cpp,ProcessLauncher_test}`.
  - `remote/HostConnector.hpp`, `host/InProcessHost`.

**Produces:**
- Proto:
  - `ProtocolVersion = 3`, `WasmPageBytes`, `memoryObjectBytes()`.
  - `MemoryGrant`, `Instantiate::memory`.
  - `GrowStatus`, `Grow`, `Grown` (kinds 16/17; the variant static_assert names `Grown`).
- POSIX `Channel`:
  - `sendWithHandle(bytes, NativeHandle)`.
  - `receiveSomeWithHandles(buffer, vector<OwnedHandle>&)`.
- Grants:
  - `HandleTarget` with `currentProcess()` and, on Windows, `ofProcess(OwnedHandle)`.
  - `HandleGrant{u64 wireValue; optional<NativeHandle> attached}`.
  - `grantHandle(target, object)`.
  - `claimGrantedHandle(PduStream&, wireValue) -> expected<OwnedHandle, StreamError>`.
- `PduStream`: `send(pdu, grant)`, `takeAttachedHandle()`. Only (Control, ToHelper) streams accept attachments, at most one.
- Process side:
  - `IProcessHandle::handleTarget() -> expected<HandleTarget, LaunchError>` (called on the supervisor thread after launch; failure → kill, `Io`).
  - `SupervisedHelper::handleTarget()`, `IHostHandle::handleTarget()`.
- Probe: `--receive-grant` reads an `Instantiate`, claims, maps, writes `probe` at 0, and reports `granted=ok|refused`.

- [ ] **Step 1: Write the transport tests** (vtextension_test, POSIX).
  - `"Channel.handleTravelsWithItsBytes"`: same `st_ino`, `FD_CLOEXEC`.
  - `"Channel.parentInstallsNoDescriptorFromTheHelper"`: 100 frames with attached pipes; the parent's open-fd count is unchanged.
  - `"PduStream.instantiateCarriesItsDescriptor"`: the second claim → `Malformed`.
  - `"PduStream.secondUnclaimedDescriptorIsMalformed"`.
  - `"PduStream.lanesNeverInstallADescriptor"`.
  - `"PduStream.claimWithoutADescriptorIsMalformed"`.
- [ ] **Step 2: Write the codec tests.**
  - Round-trip samples gain `Instantiate`+grant, `Grow`, `Grown`.
  - `"PduCodec.protocolVersionIsThree"`.
  - `"PduCodec.grantAtItsLargestFitsItsCap"` (`UINT64_MAX` ×3).
  - `"PduCodec.memoryObjectBytes"`: (0) → 64 KiB, (32 MiB + 1) → 32 MiB + 64 KiB.
  - The existing direction/channel and mutation cases cover the new rows.
- [ ] **Step 3: Write the grant tests** (process_test, wine).
  - `"HandleGrant.reachesAChild"`: `granted=ok`, and the test's mapping reads `probe` within 5 s.
  - `"HandleGrant.forgedValueIsRefused"`: Windows sends the probe's own pipe value; POSIX attaches no descriptor → `granted=refused`, exit 0.
  - `"Supervisor.launchCarriesAHandleTarget"`: a failing target → `Io`, and the fake was killed.
- [ ] **Step 4: Build** `vtextension_test vtextension_process_test extension_host_probe`; expect failure.
- [ ] **Step 5: Implement the channel and stream:**
  - `sendmsg` with one `SCM_RIGHTS` cmsg;
  - `recvmsg` with `CMSG_SPACE(2 fds)`;
  - `MSG_CTRUNC` or a foreign cmsg → close all, `Io`;
  - Windows: not compiled.
- [ ] **Step 6: Implement grants and targets** (D69). On Windows, launcher targets are `DuplicateHandle(child, PROCESS_DUP_HANDLE)`.
- [ ] **Step 7: Implement the wire rows.** HostServer ignores `memory` for now; the helper still says Copy.
- [ ] **Step 8: Run** `vtextension_test "Channel.*" "PduStream.*" "PduCodec.*"` and `vtextension_process_test "HandleGrant.*" "Supervisor.*"` → PASS; ctest → PASS.
- [ ] **Step 9: Commit** (two):
  - `feat(vtextension): hand an extension host a descriptor with the request that needs it`
  - `feat(vtextension): protocol for memory Contour shares with an extension host`

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

### Task 4.6: Single-fetch decoding
**Files:**
- Modify: `vtextension/{CanonicalAbi.hpp,Instance.cpp,ControlImports.cpp,CanonicalAbi_test.cpp}`, every `IGuestMemory` double, `process/testing/LaunchProbe.cpp` (`--mutate`).
- Create: `remote/HostileMemory_test.cpp` (helper_test).

**Produces:**
- `IGuestMemory::read(Address, span<byte> into) -> Result<void>`; `view` is renamed `writable`; `allocate` and `chargeLift` are unchanged.
- `detail::readInteger<U>(memory, address)`, `detail::readBytes<Container>(memory, list, elementSize)`.
- `GuestMemory::read` = one `memcpy` from `IWasmInstance::memory()`.

- [ ] **Step 1: Write the tests.**
  - `"GuestReader.fetchesEachByteOnce"`: a TEMPLATE over every type the host lifts (string, byte list, ConfigMap, LogLevel, each control request).
    - A counting memory stores a sample, then loads it.
    - During loads, `writable()` returns a decoy buffer.
    - Assertions: `load == sample`, and each guest byte is read at most once.
  - `"GuestReader.mutationAfterReadChangesNothing"`: `read()` corrupts its source after copying; the string stays the first copy and the list keeps its first length.
  - `"hostile memory does not break decoding"`:
    - Setup: a 1 MiB object laid out with samples; the probe, launched with `--mutate`, inherits it and flips bytes (`mt19937{0x5eed}`); the test runs 100 000 decodes.
    - Each decode either fails (`Trapped`/`OutOfMemory`) or yields valid UTF-8, in-range enums and lengths ≤ the budget.
    - The probe reports `mutations > 0` and exits 0 at EOF.
- [ ] **Step 2: Build** `vtextension_test vtextension_helper_test extension_host_probe`; expect failure.
- [ ] **Step 3: Interface, Instance and doubles.**
- [ ] **Step 4: Move every decode in `CanonicalAbi.hpp` onto `read`.**
- [ ] **Step 5: Add the probe's `--mutate`.**
- [ ] **Step 6: Run** `vtextension_test "GuestReader.*" "CanonicalAbi.*" "ControlImports.*"` and `helper_test "hostile memory does not break decoding"` → PASS; ctest → PASS.
- [ ] **Step 7: Commit** `fix(vtextension): decode what an extension passes from one copy of its memory`.

### Task 4.7: Gate
- [ ] **Step 1: ASan/LSan.** Build all five test binaries and the three executables; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass. Contract: 42 cases per composition; `uncleanExits == 0`.
- [ ] **Step 2: TSan** (`out/clang-tsan`, the same targets) → zero reports, no suppressions.
- [ ] **Step 3: Measurement** (`out/clang-release`, the two needed targets, three runs each).
  - `Process.measureRender` (steady/uncached) through: the Shared helper, `copy-memory`, the in-process Shared host, and in-process Wasmtime.
  - `Process.measureRoundTrip`, `Process.measureSpawn`, new `Process.measureInstantiate`.
  - Helper and parent VmRSS + Pss (`smaps_rollup`) and VmData after 16 plasma instances.
  - Report against phase 3: steady 18.1–18.6 µs, round trip 3.3 µs, spawn 1.2 ms, RSS 10.8 MiB idle.
  - D83: the steady median must be below 18.1 µs, or stop and report.
  - Record everything in "Phase-4 facts".
- [ ] **Step 4: Windows cross-build + wine** (`linux-to-windows-clangcl-debug`, Embedded debug format, extensions on, bundled and GUI off). Build `vtextension_process_test extension_host_probe contour-extension-host vtextension_helper_test`; five wine runs.
  - **Wine can show:** clean compiles with `-Werror`; `SEC_RESERVE` create/commit; `DuplicateHandle` into a child; `HandleGrant.*`; and the Copy path end to end.
  - **Wine cannot prove:** placeholder mappings (if `canMapGuestMemory()` is false the helper says Copy, recorded); an access violation on uncommitted pages becoming a trap; job accounting of section commits.
  - Record what ran; fix any real break, each in its own commit.
- [ ] **Step 5: FreeBSD/macOS.** Code review against a checklist:
  - XNU `ftruncate` works once;
  - macOS has no `MSG_CMSG_CLOEXEC` (`fcntl` instead);
  - `CMSG` alignment;
  - shm `MAP_FIXED` over an anonymous reservation;
  - the Rosetta bound;
  - FreeBSD `memfd` seals and its parent-only role (WAMR helper in 5.3).
  
  CI results only if the owner allows a push; otherwise record them as outstanding.
- [ ] **Step 6: Coverage** of the SharedObject, HandleGrant, GuestMemoryMapping, MemoryProvision, WasmtimeMemoryCreator, SharedGuestMemory, RemoteBackend and HostServer sources; name the kinds of uncovered lines.
- [ ] **Step 7: Phase review** (`/code-review` of `4ec0d13f6..HEAD`); address every finding.
- [ ] **Step 8: Phase summary:** performance (Step 3), risk (D69, D74, D75, D76, D81), coverage, and what wine and CI did and did not prove.

### Phase-4 facts
*(Filled in by 4.4 and 4.7.)*

## Review Focus addendum (phase 4)
6. **A helper mutates memory while Contour decodes.** No decoder may read a byte twice. Pinned by `"GuestReader.fetchesEachByteOnce"` and `"hostile memory does not break decoding"`; review rule: no decoder reads through `writable()`.
7. **Contour touching memory it never granted.** A helper's growth past the cap, shrink or wrong instance; reads past the acknowledged size; Windows uncommitted pages. Pinned by `"SharedGuestMemory.growthIsChecked"`, `"RemoteBackend.badGrowKillsTheHost"` and the grow hostility rows.
8. **Descriptor leaks either way.** Contour must install no descriptor a helper sends, and hold none per instance. Pinned by `"Channel.parentInstallsNoDescriptorFromTheHelper"`, `descriptor-up` and `"RemoteBackend.instancesHoldNoDescriptor"`.
9. **Growth on unusual paths:** during nested `cabi_realloc` lowering, while interrupted, or in a start function. Pinned by `"RemoteBackend.growDuringLoweringIsSeen"`, `"RemoteBackend.growWhileInterruptedIsRefused"`, `"HostServer.growOutsideALaneIsRefused"` and `grownMemoryIsVisible`.
10. **Unproven platforms.** Rosetta/XNU rounding, Windows placeholders and macOS ancillary handling are verified by reading and CI only. D81 also leaves guest memory outside RLIMIT_DATA.

## Task order
4.1 → 4.2 → 4.3 → 4.4 → 4.5 → 4.6 → 4.7 (4.6 depends only on 4.2 and may run beside 4.3–4.5).
