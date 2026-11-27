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


## Item 0 (carried from the previous task's review; do first, own commit)
1. `transport/SharedObject_test.cpp` ~76: add a Windows-only case that commits a `SEC_RESERVE` guest-memory object through ONE view only and then writes through a SECOND view (and reads back through the first). Tag it so it runs on real Windows (CI) but is skipped under wine (wine models commit per view) — detect wine via `ntdll!wine_get_version` and SKIP with a reason.
2. `process/ProcessLauncher_test.cpp` ~408: `REQUIRE(::poll(...) == 1)` aborts before the reap; `LinuxProcessHandle` does not kill on destruction, so a regressed launcher leaks a live child. Use CHECK and always kill+reap.
3. `host/HostProcess_test.cpp` ~485: assert the logged `hardening failed: …` line carries the failing row's own text (capture the stderr/tell sink), so rewiring `HostProcess.cpp` back to a lookup fails a test.
