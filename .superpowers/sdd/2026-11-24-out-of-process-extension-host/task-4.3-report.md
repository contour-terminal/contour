# Task 4.3 report: handing an object to the helper; protocol v3

Status: **DONE**, with the deviations listed below. Branch `feature/out-of-process-extension-host`, base `3d57fdbef`.

## Commits

| Hash | Subject |
|---|---|
| `78019b6f9` | test(vtextension): pin what three earlier tests only assumed (Item 0) |
| `aca49438e` | feat(vtextension): protocol for memory Contour shares with an extension host |
| `b056f2576` | feat(vtextension): hand an extension host a descriptor with the request that needs it |

All three are signed off and carry no AI trailers. Nothing was pushed.

## What changed

### Item 0 (`78019b6f9`)
1. `transport/SharedObject_test.cpp`: new Windows-only case `SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother`. It commits a `SEC_RESERVE` object through one view only, writes through a second view and reads back through the first. Under wine it calls `SKIP`, detected through `ntdll!wine_get_version`. It shares the `#ifdef _WIN32` block with the existing reserved-section case.
2. `process/ProcessLauncher_test.cpp` (`unsupportedPidfdWaitsRefuseEveryLaunch`):
   - The poll is now a `CHECK`.
   - A child that started anyway is always ended and reaped (`endAndReap`) before anything else can abort the case.
   - `receiveSome` runs only if the poll said the channel is readable, so a regressed launcher cannot block it.
3. `host/HostProcess{.hpp,.cpp,_test.cpp}`:
   - `HostConfinement` gains `tell`, the sink the host reports a failed hardening through (DI). `systemConfinement()` wires it to the new public `tellOnStandardError()`.
   - The test confinement records the lines it is given. `HostProcess.failedHardeningSaysWhichStep` now fails with step `Tracing` and the text `"the third row"`, and asserts the exact line `hardening failed: the third row (<strerror(1)>)`.

### Protocol v3 (`aca49438e`): `proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`, `RemoteBackend.cpp`
New and changed protocol items:
- `ProtocolVersion = 3`, `WasmPageBytes`, and `constexpr memoryObjectBytes(cap)`.
- `MemoryGrant{u64 handle, objectBytes, initialBytes}` as `Instantiate::memory`. It has a default member initializer, so the existing designated initializers stay valid under `-Wmissing-designated-field-initializers`. The Instantiate cap goes from 32 to 64.
- `GrowStatus{Refused, Granted}`, `Grow{u32 instanceId, u64 bytes}` (cap 16) and `Grown{GrowStatus}` (cap 8), as kinds 16 and 17. The variant `static_assert` now names `Grown`.

**R29 shape:**
- `PduRow::channel` became `PduRow::channels`, of type `ChannelSet : uint8_t`. It is a bitmask with one bit per `ChannelKind`: `Control`, `Lane`, `Either`.
- `travelsOn(row, channel)` is the one predicate, and `readHeader` uses it.
- Grow (ToParent) and Grown (ToHelper) are `Either`; every other row names exactly one channel.

`RemoteBackend` sends `.memory = {}` explicitly, which means objectBytes 0, a copy-mode host. HostServer ignores `memory`, and the helper still says `Copy`.

### Handle passing (`b056f2576`)
**`transport/Channel{.hpp,_posix.cpp}`** (POSIX only):
- `sendWithHandle(bytes, NativeHandle)`: one `sendmsg` carrying an `SCM_RIGHTS` cmsg of `CMSG_SPACE(sizeof(int))`, then plain `send` for the rest.
- `receiveSomeWithHandles(buffer, vector<OwnedHandle>&)`:
  - `recvmsg` with a buffer of `CMSG_SPACE(2 * sizeof(int))`.
  - `MSG_CMSG_CLOEXEC` where it is defined (Linux, FreeBSD); otherwise (macOS) `fcntl(FD_CLOEXEC)` right after `recvmsg`.
  - `MSG_CTRUNC`, a cmsg that is not `SCM_RIGHTS`, or a failed `fcntl` closes every descriptor received (they are `OwnedHandle` at once) and returns `Io`.
  - `msg_controllen` and `cmsg_len` are cast through `decltype` aliases, because their types differ between Linux and macOS (the same idiom as `vthost/keeper/DescriptorSocket.cpp`).

**`transport/PduStream`:**
- `send(pdu, HandleGrant const&)`. Both `send` overloads go through one private `sendAttaching()`.
- `takeAttachedHandle()`.
- `Attachments{Discarded, Taken}` is fixed at construction. It is `Taken` only for (Control, ToHelper) on POSIX; on Windows nothing travels beside a frame.
- A taken stream holds at most one unclaimed descriptor. A second one, or two in one receive, gives `Malformed` (the new one is closed). Every other stream keeps reading with plain `recv()` (D70).

**`transport/HandleGrant{.hpp,_posix.cpp,_win32.cpp}`:**
- `HandleTarget` is move-only. It has `currentProcess()` and, on Windows, `ofProcess(OwnedHandle)` and `process()`.
- `HandleGrant{u64 wireValue; optional<NativeHandle> attached}`.
- `GrantError{Io}`. This enum is new: `grantHandle` needed an error type and the brief named none.
- `grantHandle(target, object)`:
  - POSIX: `{0, object}`.
  - Windows: `DuplicateHandle(..., FILE_MAP_READ|FILE_MAP_WRITE)` into `target.process()`.
- `claimGrantedHandle(stream, wireValue) -> expected<OwnedHandle, StreamError>`:
  - POSIX: takes the held descriptor first (so a refused claim leaves nothing behind). A missing descriptor or a value other than 0 gives `Malformed`.
  - Windows: `Malformed` if the stream holds anything, or if the value is 0 or wider than `uintptr_t`. Otherwise it returns the value as a handle; the caller must release it, not close it, if it cannot map it.

**Process side:**
- `IProcessHandle::handleTarget() -> expected<HandleTarget, LaunchError>`:
  - Linux, macOS and FreeBSD return `currentProcess()`.
  - Win32 returns `DuplicateHandle(process, PROCESS_DUP_HANDLE)` wrapped by `ofProcess`.
  - `RecordingLauncher` delegates. The test's `CopiedWaitable` delegates too.
  - `FakeProcessLauncher` gains `failHandleTargets(LaunchError)` and `handleTargetThreads()`.
- `SupervisorLoop::launch` calls it on the supervisor thread right after launch, before `watch`. On failure the helper is killed (`refuseLaunched()`, the refused-watch path, now shared) and the launch returns `Io`.
- `SupervisedHelper::handleTarget()` holds the target.
- `IHostHandle::handleTarget()`:
  - `ProcessHostHandle` returns `SupervisedHelper`'s target.
  - `InProcessHost` returns its own `currentProcess()` target.
  - The `StampingHandle` and `CountingHandle` test doubles are updated.

**Probe:** `extension_host_probe --receive-grant <helper args>` does the following:
1. Binds to its parent, adopts control as a (Control, ToHelper) `PduStream`, and reads one Instantiate.
2. Claims the handle, maps it with `SharedMapping` (`objectBytes`, ReadWrite), commits the first page and writes `probe` at 0. On Windows a value it cannot map is `release()`d.
3. Reports `granted=ok|refused` and `end` on its **first lane**, then waits for control to end and exits 0.

**Tidy-ups:**
- `readReport` moved from `ProcessLauncher_test.cpp` into `process/testing/LaunchedHelper` and is shared with the grant test.
- `inodeOf` and `openDescriptorCount` were added to `transport/testing/HandleCopies.hpp` (POSIX).

## Tests, and how each was seen failing
I wrote the implementation before most tests, not strictly test-first. To see each test fail, I injected faults into the finished code, rebuilt and ran it, then restored the files byte for byte (checked with `cmp`).

| Test | How I saw it fail |
|---|---|
| `Channel.handleTravelsWithItsBytes` | `ReceiveFlags = 0` (no `MSG_CMSG_CLOEXEC`) → the `FD_CLOEXEC` check fails |
| `Channel.parentInstallsNoDescriptorFromTheHelper` | Every stream `Attachments::Taken` → the 2nd frame is `Malformed` (an unclaimed descriptor) |
| `PduStream.instantiateCarriesItsDescriptor` | `grantHandle` POSIX attaches nothing → the claim is `Malformed` |
| `PduStream.secondUnclaimedDescriptorIsMalformed` | The `&& _attached` check removed → the second receive succeeds |
| `PduStream.lanesNeverInstallADescriptor` | Every stream `Taken` → the lane holds a descriptor |
| `PduStream.claimWithoutADescriptorIsMalformed` | The claim ignores `wireValue != 0` → the 0x1F4 claim succeeds |
| `PduCodec.roundTrip` / `everyPrefixNeedsMoreData` / mutation (new samples) | Run green with the new samples. The channel case below covers the rows |
| `PduCodec.directionAndChannelAreChecked` (now every sample × both channels × both directions, plus the Grow/Grown `Either` asserts) | Grow's row set to `Lane` → fails |
| `PduCodec.protocolVersionIsThree` | `STATIC_CHECK`; fails at compile with 2 (by construction, not run) |
| `PduCodec.grantAtItsLargestFitsItsCap` | Instantiate cap back to 32 → fails |
| `PduCodec.memoryObjectBytes` | The "at least one page" clause removed → `STATIC_CHECK` fails at compile |
| `HandleGrant.reachesAChild` | `grantHandle` POSIX attaches nothing → `granted=refused`, and the mark is never seen |
| `HandleGrant.forgedValueIsRefused` | **Not seen failing.** No single-site fault makes a forged grant map on POSIX: the size check behind `SharedMapping::map` refuses it as well. It passes on Linux and under wine; under wine it exercises the release-not-close path, since the probe still hears its control channel end |
| `Supervisor.launchCarriesAHandleTarget` | The target failure ignored (replaced by `currentProcess()`) → the launch succeeds and nothing is killed |
| Item 0: `HostProcess.failedHardeningSaysWhichStep` | `HostProcess.cpp` rewired to look the text up again (`hardeningSteps().front().text`) → the told line differs. A lookup by step (`find`) overflowed under ASan on Linux, because `Tracing` is not a Linux row |
| Item 0: `ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch` | Not seen failing: it needs a regressed launcher. The change is structural: the CHECK, the unconditional reap, and the receive guarded by readiness |
| Item 0: `SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother` | Skipped under wine by design. Not run on real Windows here; Windows CI will run it |

## Results
- **Focused tests, final tree (Linux, clang-asan):**
  - `vtextension_test "Channel.*,PduStream.*,PduCodec.*"`: 37 cases, 19547 assertions, all passed.
  - `vtextension_process_test "HandleGrant.*,Supervisor.*"`: 33 cases, 233 assertions, all passed.
  - Full `vtextension_process_test`: 725 assertions, all passed.
  - Item 0: `vtextension_remote_test "HostProcess.*,HostBootstrap.*"`: 17 cases, 253 assertions; process `"ProcessLauncher.*,SharedObject.*"`: 18 cases, 169 assertions.
- **`ctest --preset=clang-asan -R vtextension`: 5/5 passed.**
  - I ran it on the protocol commit alone, with the handle-passing work stashed (about 40 s), and again on the final tree before the last commit.
  - The Item 0 commit was made after its focused runs only. The ctest runs that came later cover its files.
- **Windows cross-build** (`out/clangcl-debug`, clang-cl, `-Werror`), targets `vtextension_process_test`, `extension_host_probe`, `contour-extension-host`, `vtextension_remote_test` and `vtextension_helper_test`: clean on the final tree. The first attempt failed on `-Wunused-private-field` (`_attachments`). That led to `attachmentsOf()`: Windows streams are `Discarded`, and the taken branch is `std::unreachable()` there.
- **wine 11.20** (own prefix `scratchpad/wine43`, mingw `zlib1.dll` on `WINEPATH`):

  | Run | Result |
  |---|---|
  | `vtextension_process_test.exe` (full, final tree) | 89 cases: 88 passed, 1 skipped (the wine-skipped cross-view case); 646 assertions |
  | `vtextension_process_test.exe "HandleGrant.*"` (final tree) | 2 cases, 31 assertions, all passed |
  | `vtextension_process_test.exe "HandleGrant.*,SharedObject.*,Supervisor.*,ProcessLauncher.*"` (before the last tidy edits) | 49 cases: 48 passed, 1 skipped |
  | `vtextension_remote_test.exe "HostBootstrap.*,HostProcess.*"` | 15 cases, 242 assertions, all passed |
  | `vtextension_helper_test.exe "~[.measure]"` | 124 cases: 122 passed, 2 skipped; 2882 assertions |

  The remote and helper wine runs were made one rebuild before the final tree. The edits after them were a lambda parameter, a using-declaration and a doc comment.
- **clang-tidy:**
  - Every changed Linux `.cpp` (`-p out/clang-asan`) is clean once two findings were fixed:
    - `rvalue-reference-param-not-moved` on the supervisor's `transform` lambda (it now takes the value);
    - an unused `using transport::Channel` left by moving `readReport`.
  - What remains is pre-existing or not about these changes: `pro-type-member-init` on aggregates in `Pdu.hpp`, `WasmRuntime.hpp` and `ExtensionError.hpp` (none of the new structs); macOS and FreeBSD sources that do not compile here.
  - Windows `.cpp` (`-p out/clangcl-debug`, `HandleGrant_win32.cpp`, `Process_win32.cpp`, `PduStream.cpp`, both grant and shared-object tests): only pre-existing findings at `Process_win32.cpp:121,199` and `Wire.hpp:189`, none on changed lines.
- `/usr/bin/clang-format --dry-run --Werror` and `editorconfig-checker` on every changed file: clean.
- **macOS and FreeBSD:** not compiled; checked by reading only.
  - macOS has no `MSG_CMSG_CLOEXEC`, so the `#ifndef` branch sets `FD_CLOEXEC` after `recvmsg`.
  - `CMSG_SPACE`, `CMSG_LEN` and `CMSG_DATA` are used throughout, and the buffers are `alignas(cmsghdr)`.
  - The `msg_controllen` and `cmsg_len` types come from `decltype`.
  - FreeBSD takes the `MSG_CMSG_CLOEXEC` path.
  - `handleTarget()` was added to `MacProcessHandle` and `FreeBsdProcessHandle`.
- **Performance:** the lane and parent paths are unchanged (plain `recv`). The helper's control stream now uses `recvmsg` with a 24-byte control buffer, on the control channel only and not per call. Instantiate is now up to 30 bytes larger.

## Deviations
1. **Commit order is swapped:** the protocol commit comes first, then handle passing. The handle tests and the probe read `Instantiate::memory`, so handle passing cannot build before the protocol does. Each commit builds and passes ctest on its own.
2. **One `PduRow` names a channel set** (`ChannelSet` plus `travelsOn`) instead of adding a row per channel. A row stays one per kind, so the kind-indexed table and its `static_assert`s hold. R29's "a Control row in each direction" is expressed as Grow and Grown each being `Either`.
3. **`GrantError{Io}` is new.** The brief gave `grantHandle` no return type.
4. **The probe reports on its first lane, not on control.** Its control channel is a `PduStream` that owns the channel, so no raw text can be written there.
5. **Item 0.3 is done through DI** (`HostConfinement::tell`) rather than by capturing stderr. As a result, `tellOnStandardError()` became public in `HostProcess.hpp`.
6. **`readReport` moved** to `process/testing/LaunchedHelper` so the grant test can share it.
7. **`/simplify` was not run as a skill:** the brief forbids subagents. I did the review myself and folded it into the commits: both `send` paths share `sendAttaching()`, `refuseLaunched()` is shared, and `readReport` and `inodeOf` are deduplicated.

## Concerns
1. **macOS and BSD stream coalescing.** Linux's `recvmsg` stops after the message that carries descriptors. Whether XNU or FreeBSD may merge two descriptor-carrying sends into one receive was not verified. If one does, the stream reports `Malformed` (two descriptors) rather than leaking anything, which is safe. The parent never has two Instantiates in flight on control (requests are serialized), and `secondUnclaimedDescriptorIsMalformed` sends one at a time so the test does not depend on this. As `vthost/keeper/DescriptorSocket.cpp` notes, macOS also installs descriptors that do not fit the control buffer and leaks them. A hostile peer could therefore leak descriptors into the helper (never into Contour) before the stream ends. This touches only the helper, which dies on the violation.
2. **On Windows, granting to `currentProcess()`** (the in-process host, or tests) puts a duplicate into Contour's own process that only a claim closes. HostServer ignores `memory` for now, so nothing sends a grant in production yet. Task 4.5 must close the duplicate if the in-process host never claims it.
3. **A moved-from `SupervisedHelper`** keeps a moved-from `HandleTarget`, whose `process()` on Windows falls back to `GetCurrentProcess()`. Such an object is only destroyed or reassigned, so this does not matter today. It is documented on neither side.
4. **The Windows claim does not verify the value**; the mapping does. Every caller has to follow the release-not-close rule. Only the probe uses the claim today; the helper (task 4.4) must follow the same rule. A shared `mapGrantedObject()` may be worth adding then.
5. **The cross-view commit case** (Item 0.1) runs only on real Windows CI. If Windows needed a commit through each view after all, it fails there. The 4.2 report raised this same open question.
6. **`HandleGrant.forgedValueIsRefused` was never seen failing** (see the table), and neither was the reworked pidfd launcher test.

## Fix round 1

Commits, on top of `b056f2576`. Both are new commits, not fixups, so the reviewed history stays as it was.

| Hash | Subject |
|---|---|
| `ec5bd6e2d` | fix(vtextension): size no memory object at 0 for a cap near the top (finding 5) |
| `4b7ebf90d` | fix(vtextension): refuse a forged grant without closing what it names (findings 1–4, 6) |

### Findings
1. **Refused ancillary data is now `Malformed`.**
   - `ChannelError` gains `Refused`. `receiveSomeWithHandles` returns it for `MSG_CTRUNC` or a cmsg that is not `SCM_RIGHTS`, and closes every descriptor received.
   - A failed `fcntl(FD_CLOEXEC)` on macOS stays `Io`: that is a failure of this process, not of the peer.
   - `PduStream` maps `Refused` to `Malformed`; `arrived.size() > 1` was already `Malformed`.
   - New tests:
     - `PduStream.tooManyDescriptorsAreMalformed`: one `SCM_RIGHTS` cmsg carrying 2 descriptors, then one carrying 3, sent with a raw `sendmsg`.
     - `PduStream.foreignAncillaryDataIsMalformed`: Linux, with `SO_PASSCRED` set on the reader.

     Each asserts `Malformed` and an unchanged open-descriptor count.
   - The credentials cmsg (`CMSG_SPACE(12)` = 32 bytes) does not fit the 24-byte receive buffer. On Linux the case therefore ends through `MSG_CTRUNC`, not through the not-`SCM_RIGHTS` check, and that check is not driven by any test. Enlarging the buffer to make room would let three descriptors fit as well, and would defeat the 2-descriptor bound.
   - Seen failing: with the `arrived.size() > 1` check deleted, the 2-descriptor case fails. With `Refused` mapped to `Io`, the 3-descriptor case and the foreign case fail.
2. **The forged-grant test now detects a closed control pipe.**
   - After `granted=refused`, the test sends a `Release` on control. The probe reports `heard` and `end` on its lane for the first PDU it receives after the grant, and the test asserts it. If the forged value had been closed, the probe's control would be gone, it would report nothing, and `readReport` would fail.
   - The POSIX branch carries a comment that it pins only the end-to-end outcome.
   - Not seen failing on Windows: running a mutated wine build would have meant a second wine run, and the brief asked for one.
3. **Claims are safe by default.** `claimGrantedHandle` returns a `GrantedHandle` (move-only):
   - `get()` returns the handle for mapping.
   - `adopt() &&` returns an `OwnedHandle` once mapping has proved the handle is the object.
   - Destroying it unadopted closes a POSIX descriptor, which is always the receiver's own, and forgets a Windows value without closing it.

   I chose a claim type over `mapGrantedObject()` because task 4.4's helper maps the object into the engine's own reservation (`MAP_FIXED`, Windows placeholders; D73 and D74), not through `SharedMapping`. A one-step map would not serve it.

   On Windows, `claimGrantedHandle` also rejects values that are not multiples of 4, and pseudo-handles: any value that is negative as an `intptr_t`. The probe now adopts only after a successful map.

   New Windows tests:
   - `HandleGrant.claimRefusesWhatNamesNoHandle`: 0, 0x1F5, -1, -2, -4.
   - `HandleGrant.unprovedClaimIsNeverClosed`: one of this process's own event handles stays open after an unadopted claim, and closes after `adopt()`.
4. **`HandleTarget` has explicit states.**
   - `Kind{None, CurrentProcess, OtherProcess}`; `OtherProcess` exists only on Windows.
   - Construction:
     - `none()` is a new factory, and `isNone()` reports the state.
     - The move constructor and move assignment leave `None` behind.
   - Granting:
     - `process()` returns an optional on Windows.
     - `grantHandle` returns `Io` for a `None` target, on both platforms.
   - Test: `HandleGrant.noTargetGrantsNothing`. It checks that `none()` grants nothing and that a moved-to target still grants. The moved-from object itself is not touched, because that would trip `bugprone-use-after-move`.
5. **`memoryObjectBytes` clamps.** It clamps to `MaxMemoryObjectBytes = (UINT64_MAX / WasmPageBytes) * WasmPageBytes`, so the result is never 0. New `STATIC_CHECK`s cover `UINT64_MAX`, the value one above the maximum, and the maximum itself.
6. **The self-grant obligation is documented** on `HandleTarget::currentProcess()`: on Windows a duplicate only a claim closes, which the granter must see claimed or close itself. The code is unchanged.

### Results
- **Focused tests:**
  - `vtextension_test "Channel.*,PduStream.*,PduCodec.*"`: 39 cases, 19575 assertions, all passed.
  - `vtextension_process_test "HandleGrant.*,Supervisor.*"`: 34 cases, 241 assertions, all passed.
- `ctest --preset=clang-asan -R vtextension`: 5/5 passed.
- **Windows:**
  - Cross-build (`out/clangcl-debug`, the five targets): clean.
  - wine, full `vtextension_process_test.exe`, run once: 92 cases, 91 passed, 1 skipped (the wine-skipped cross-view case); 668 assertions.
- **clang-tidy:** nothing new on the changed files.
  - Linux (`out/clang-asan`): only the pre-existing `pro-type-member-init` noise.
  - Windows (`HandleGrant_win32.cpp`, `HandleGrant_test.cpp`, `LaunchProbe.cpp`): only the pre-existing `pro-type-member-init` noise and `readability-duplicate-include`, which this report already notes as a clang-cl artefact.
- clang-format `--dry-run --Werror` and editorconfig-checker: clean.
