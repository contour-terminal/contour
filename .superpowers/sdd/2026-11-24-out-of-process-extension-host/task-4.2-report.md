# Task 4.2 report: shared objects, one creation path

Status: DONE. Three commits on `feature/out-of-process-extension-host`, on top of BASE `0e71d554d`. Nothing was pushed.

## Commits

| Commit | Subject |
|---|---|
| `1803ef305` | `fix(vtextension): name every failure in full, and prove a refused launch` (Item 0) |
| `46f447648` | `fix(vtextension): accept a shared object rounded up to any page a parent may use` |
| `3d57fdbef` | `feat(vtextension): reserved shared objects whose pages Contour commits` |

## Item 0 (`1803ef305`)

1. **`ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch`.** `process/testing/LaunchedHelper` now has `prepareHelperLaunch(HelperLaunch) -> PreparedHelperLaunch`, which holds the spec and both ends of every channel, the page and the wake signal. `launchHelper()` is built on it. The test now:
   - starts the probe with the same arguments `launchProbe` uses (`--report` plus the full helper arguments);
   - keeps the launch result alive until the end;
   - drops every helper end;
   - waits on the control channel with `poll()` for at most `ChildPatience`, then expects `Closed`.
   If a child was started anyway, the test reaps it.
2. **The failed hardening step's text.** `hardenProcess(span)` now returns `std::expected<void, HardeningFailure>`, where `HardeningFailure` is `{step, text, code}`, taken from the row that ran. `hardeningStepText()` is removed. A new per-OS `systemErrorText(int)` gives `std::system_category().message` on POSIX, and the same message plus `HRESULT 0x…` on Windows. `hardeningFailureText()` produces `"<text> (<system text>)"`. The helper logs `hardening failed: <that>`.
3. **Exhaustive `HostRefusal`.** The header now declares, next to the enum:
   - `LastHostRefusal`;
   - `RefusalSource { None, Launch, Handshake }`;
   - `constexpr refusalSource()`, a switch with no default.
   Two `static_assert`s pin `LastHostRefusal`: it maps to a source, and `Last + 1` maps to `None`. Adding a reason after `MemoryMode` therefore fails to compile until `LastHostRefusal` names it. A `consteval` check confirms that every handshake reason has a row. `refusalLine()` dispatches on the source and never returns an empty line; a launch reason with no launcher row gets a generic line. The test loop and the log's bit-width assert both use `LastHostRefusal`.

## Task 4.2

New files under `src/vtextension/wasm/remote/transport/`:
- `OwnedHandle.hpp`: `get`, `release`, `operator bool`, and `NoNativeHandle`.
- `SharedObject.hpp`, which declares:
  - `MaxPageBytes = 16 KiB`;
  - `constexpr fitsSharedObject` (overflow-free);
  - `SharedObjectUse { GuestMemory, InterruptPage }`;
  - `MapAccess { ReadOnly, ReadWrite }`;
  - `SharedObjectError`, moved here from ControlPage and extended with `OutOfRange`;
  - `createSharedObject`, `sealSharedObject`, `checkSharedObjectSize` (POSIX) and `SharedMapping::{map, bytes, commit}`.
- `SharedObject.cpp`: the parts every OS shares (mapping ownership, move operations, the range check in `commit`).
- `SharedObject_posix.cpp`: a per-use table `{use, memfdName, laterWrites}`.
  - Linux and FreeBSD: `memfd_create(MFD_CLOEXEC|MFD_ALLOW_SEALING)`, then `ftruncate`, then `F_SEAL_SHRINK|F_SEAL_GROW` at creation.
  - `sealSharedObject` adds `F_SEAL_FUTURE_WRITE` only for `InterruptPage`, tolerating `EINVAL` from old kernels. It then adds `F_SEAL_SEAL`.
  - macOS: `/contour-eh-<pid>-<serial>` with `O_EXCL`, unlinked at once, `FD_CLOEXEC`, a single `ftruncate`, no seals.
  - `map` runs `checkSharedObjectSize` before it calls `mmap(MAP_SHARED)`.
- `SharedObject_win32.cpp`: a per-use table `{use, sectionKind}`. `GuestMemory` uses `SEC_RESERVE` and `InterruptPage` uses `SEC_COMMIT`. `map` maps the whole view and measures its extent with `VirtualQuery`, summing every region with the same `AllocationBase`. It refuses the object with `WrongSize` unless `fitsSharedObject` accepts the extent, and exposes only the first `bytes`. `commit` calls `VirtualAlloc(MEM_COMMIT)` through the view.
- `SharedObject_test.cpp` is in `vtextension_process_test`.

Modified:
- `ControlPage.hpp`: `isControlPageSize` is removed. The page now holds an `OwnedHandle` plus a `SharedMapping`, and its moves and destructor are defaulted.
- `ControlPage_posix.cpp` and `ControlPage_win32.cpp`: both are now thin wrappers over `SharedObject`.
  - POSIX `adopt` goes through `SharedMapping::map`, which calls `checkSharedObjectSize`. This fixes M1.
  - Windows `create` still narrows the handle it keeps to `FILE_MAP_READ`.
- `Channel.hpp` gains `closeNativeHandle(NativeHandle)`, defined in `Channel_posix.cpp` and `Channel_win32.cpp`.
- CMake: the new sources and the test.
- `ControlPage_test.cpp`: `ControlPage.sizeMayBeRoundedUpToThePage` is deleted. See deviations 3 and 4 for the other test changes.

## Tests, and how each was seen to fail

I wrote the tests alongside the implementation, not strictly before it. So "fail first" was demonstrated by mutation: I broke the code, rebuilt, ran the test, and restored the code. Every mutation below made its test fail.

| Test | Mutation → result |
|---|---|
| `SharedObject.sizeMayBeRoundedUpToTheLargestPage` (STATIC_CHECKs) | bound by 4096 (the reader's page) → compile error `static assertion failed … fitsSharedObject(16 * KiB, 4 * KiB)` |
| `SharedObject.writesAreSeenThroughASecondMapping` | `MAP_PRIVATE` → 3 cases failed (this one, ControlPage raise/helperView) |
| `SharedObject.isSealedAgainstResizing` | no `F_SEAL_GROW` → 2 failed; `F_SEAL_FUTURE_WRITE` on guest memory too → 1 failed; no `F_SEAL_SEAL` → 1 failed |
| `SharedObject.isNotInherited` | no `MFD_CLOEXEC` → 2 failed (this, `ControlPage.isNotInherited`) |
| `SharedObject.checkRefusesAWrongSize` | `map` without the size check → 2 failed (this, `ControlPage.adoptRefusesAWrongSize`) |
| `SharedObject.reservedSectionCommitsOnlyWhatIsAsked` (Windows) | `GuestMemory` → `SEC_COMMIT`, under wine → 3 CHECKs failed (State, RegionSize, State) |
| `SharedObject.noObjectIsEmpty` (extra) | not mutated; guards the 0-size refusal |
| `ControlPage.adoptAcceptsThePageRoundedUpToTheLargestPage` (extra, POSIX; the M1 case end to end) | size check bounded by `sysconf(_SC_PAGESIZE)` (the old rule) → this case failed, 12/13 passed |
| `ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch` | launcher swapped for `makeSystemProcessLauncher()` (spawns) → failed at both CHECKs (launch result, and the report bytes instead of `Closed`), no hang |
| `HostRefusals` exhaustiveness | an extra enumerator `Extra` after `MemoryMode`, handled in the switch but `LastHostRefusal` not updated → compile error `LastHostRefusal must name the last reason` |
| `HostBootstrap.failureSaysWhatTheSystemSaid` (new), updated `hardeningNamesTheFirstFailedStep` and `failedHardeningSaysWhichStep` | compile failure only against the old API: `HardeningFailure` and `hardeningFailureText` did not exist. No behavioural mutation was run. |

## Results

- **ASan (`out/clang-asan`):**
  - `vtextension_process_test "SharedObject.*,ControlPage.*"`: 13 cases, 88 assertions, all passed.
  - Item 0 focused runs: process test 5 cases / 67 assertions; remote test `HostBootstrap.*,HostProcess.*` 17 cases / 251 assertions. All passed.
- **`ctest --preset=clang-asan -R vtextension`:** 5/5 passed (about 41 s). I ran it before each of the three commits, each time against that commit's own tree.
- **Windows cross-build (`out/clangcl-debug`, clang-cl, `-Werror`):** clean. Targets: `vtextension_process_test`, `extension_host_probe`, `contour-extension-host`, `contour-extension-host-hostile`, `vtextension_helper_test`, `vtextension_remote_test`.
- **wine 11.20**, own prefix `scratchpad/wine42`, mingw `zlib1.dll` on `WINEPATH`:

  | Run | Result |
  |---|---|
  | `vtextension_process_test.exe "SharedObject.*,ControlPage.*"` | 10 cases / 62 assertions, all passed |
  | `vtextension_process_test.exe` (full, final tree) | 85 cases / 606 assertions, all passed |
  | `vtextension_remote_test.exe "HostBootstrap.*,HostProcess.*"` | 15 cases / 240 assertions, all passed |
  | `vtextension_helper_test.exe "~[.measure]"` | 124 cases: 122 passed, 2 skipped; 2882 assertions |
  | first-commit state, `"SharedObject.*,ControlPage.*"` | 9 cases / 47 assertions, all passed |

  I ran the helper and remote wine runs before a one-line `OwnedHandle.hpp` fix that clang-tidy asked for: `NoNativeHandle` became `auto = nullptr` on Windows. After that fix I re-cross-built and re-ran only the process test. `wineserver -k` was run on my own prefix only.
- **clang-tidy** on every changed `.cpp`:
  - Linux (`-p out/clang-asan`): clean, after fixes for `implicit-widening`, `math-missing-parentheses` and `casting-through-void`. The flag access in ControlPage now uses `reinterpret_cast`.
  - Windows `.cpp` (`-p out/clangcl-debug`): clean apart from `readability-duplicate-include`, which fires on files I did not touch as well (`ExtensionError.hpp`, `WasmRuntime.hpp`) and so looks like a clang-cl artefact. The one real finding, `misc-misplaced-const` on `NoNativeHandle`, is fixed.
- `/usr/bin/clang-format --dry-run --Werror` and `editorconfig-checker` on every changed file: clean.
- **macOS and FreeBSD:** not compiled. I checked those paths by reading only:
  - macOS: shm_open with `O_EXCL`, unlinked at once, a single `ftruncate`, no seals.
  - FreeBSD: memfd with `MFD_ALLOW_SEALING`. `F_SEAL_FUTURE_WRITE` is guarded by `#ifdef`, so FreeBSD gets `SHRINK|GROW|SEAL`.
- **Performance:** no hot path changed. ControlPage still maps once; raising and clearing a flag is the same atomic store. One `fstat` was added to the mapping path on POSIX.
- **Coverage:** not measured; there was no coverage-tree run. Every new branch has a test, except the macOS/FreeBSD-only lines, `Unsupported` errno mapping, and the system-refusal (`Io`) paths.

## Deviations, each with its reason

1. **New `transport/SharedObject.cpp`**, not on the file list. It holds the `SharedMapping` constructor, move operations, destructor and the range check in `commit()`, which are the same on every OS. Putting them here avoids a copy in each of `_posix` and `_win32`, following the `HostBootstrap.cpp` precedent. The per-OS parts are the private `commitRange()` and `release()`.
2. **`SharedObjectError::OutOfRange`**, a new enumerator, returned by `commit()` when the range runs past the mapping. **`WrongSize` is also returned for a 0-byte object**, and for one larger than `off_t` or `size_t` can hold.
3. **`ControlPage.adoptRefusesAWrongSize` changed.** It used `4 × page = 16 KiB` as "too large", which the new 16 KiB bound accepts on 4 KiB-page systems. It now uses `2 × MaxPageBytes` (32 KiB). The brief said the other ControlPage cases were unchanged, but this one could not stay unchanged.
4. **Extra tests:** `ControlPage.adoptAcceptsThePageRoundedUpToTheLargestPage` (POSIX; the M1 case through ControlPage itself), `SharedObject.noObjectIsEmpty` and `HostBootstrap.failureSaysWhatTheSystemSaid`.
5. **`SharedMapping::map` checks the size on POSIX too**, not only Windows. It does this by calling `checkSharedObjectSize`, so `adopt` gets the check by going through `map`. I chose this so that both OSes refuse a mapping of the wrong size in the same place.
6. **`sealSharedObject` is a no-op on Windows.** The interrupt page's handle narrowing (`FILE_MAP_READ`) stays in `ControlPage_win32.cpp`, because it is about which handle gets handed over, not about creating the object.
7. **Item 2's error code** is printed on Windows as both the system message and the `HRESULT` hex value. `std::system_category()` does not know every HRESULT.
8. **`/simplify` was not run.** It dispatches agents, and the brief forbids subagents. I did a self-review in its place: ControlPage's moves and destructor became `= default`, and the shared mapping code is factored out.

## Concerns

1. **Commits on Windows views made before the commit.** `writesAreSeenThroughASecondMapping` commits through *both* views. On real Windows, a helper view mapped *before* the parent commits section pages may still need its own `VirtualAlloc(MEM_COMMIT)` through that view before it can touch those pages; wine also tracks committed state per view at map time. Tasks 4.4 and 4.5 (helper `open(n)` / parent growth) must decide who commits through which view. Wine cannot settle this; Windows CI has to.
2. **`viewBytes` relies on every region of a view sharing its `AllocationBase`.** That holds for `MapViewOfFile`. Placeholder mappings (`MapViewOfFile3`, D74) may need a different extent check when GuestMemoryMapping arrives.
3. **macOS and FreeBSD are uncompiled.** In particular:
   - the macOS `openObject` chain (`succeeded(fcntl).transform`);
   - the `#ifndef __APPLE__` `and_then` that splits the expression in `createSharedObject`.
   Both read correctly, but neither has been built.
4. **`SharedObjectUse` puts `GuestMemory` first**, as the brief names it verbatim. It is not a negative/positive pair, so the "zero is the negative case" rule does not apply to it.
