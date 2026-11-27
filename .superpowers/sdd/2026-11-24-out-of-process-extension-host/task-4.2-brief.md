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


## Item 0 (carried from the review of the previous task; do first, own commit `test(vtextension): …`/`fix(vtextension): …` as fits)
1. `ProcessLauncher_test.cpp` "ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch": the "no child holds the other end" half proves nothing and would hang on regression. Keep the launch result alive and give the probe the full arguments `launchProbe` uses (so a wrongly started child writes a report), with a bounded receive — or drop that half. Must fail (not hang) against a launcher that spawns anyway.
2. `HostProcess.cpp` ~193: the failed hardening step's text is looked up in `hardeningSteps()` rather than in the rows that ran → blank `hardening failed: ` for a step this OS lacks. Return the failed row (or its text) from `hardenProcess(span)`; append the returned error code's text (strerror / HRESULT) to the line so the `int` is read.
3. `HostRefusals.cpp` ~968 and `HostRefusals_test.cpp` ~1123 both hard-code `MemoryMode` as the last reason; `refusalLine` returns "" for an unknown one. Add a `Count`-free exhaustiveness check (e.g. a switch with no default, or a table indexed and size-asserted against an explicit last-enumerator constant declared next to the enum) so a new reason cannot slip by, and never log an empty line. (D78 later removes `MemoryMode`.)
