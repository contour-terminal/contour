# Task 3.1 report — process launcher, process handles, inherited handles

Status: DONE_WITH_CONCERNS (concerns are about what only CI can verify: macOS/FreeBSD).

## Commits (on 3abef4e10)
- 64ecc9a46 `feat(vtextension): inheritable channels, an interrupt page and a wake signal for the extension host`
  — `Channel::adopt` (POSIX + Windows), `ControlPage` (posix/win32), `WakeSender`/`WakeReceiver`/`makeWakePair`
  (posix/win32), `HelperArguments` (table-driven), `MaxLaneChannels` moved to `HelperArguments.hpp`
  (`HostConnector.hpp` includes it), new `vtextension_process_test` with the transport tests. Built and
  tested on its own (11 cases) before committing.
- 92f80ee13 `feat(vtextension): launch and reap extension host processes through process handles`
  — `ProcessLauncher.hpp`, `Process_{linux,freebsd,macos,win32}.cpp`, `PosixLaunch.{hpp,cpp}`,
  `testing/{FakeProcessLauncher.{hpp,cpp},RecordingLauncher.hpp,LaunchProbe.cpp}`, `ProcessLauncher_test.cpp`,
  target `vtextension_remote_process` (source chosen by `CMAKE_SYSTEM_NAME`), `extension_host_probe`,
  `VTEXTENSION_PROBE_PATH`.

## Files (all under src/vtextension/wasm/remote/)
- transport/: Channel.hpp, Channel_posix.cpp, Channel_win32.cpp, Channel_test.cpp (modified);
  ControlPage.hpp, ControlPage_posix.cpp, ControlPage_win32.cpp, ControlPage_test.cpp,
  WakeSignal.hpp, WakeSignal_posix.cpp, WakeSignal_win32.cpp, WakeSignal_test.cpp,
  HelperArguments.hpp, HelperArguments.cpp, HelperArguments_test.cpp (new)
- process/: ProcessLauncher.hpp, PosixLaunch.hpp, PosixLaunch.cpp, Process_linux.cpp, Process_freebsd.cpp,
  Process_macos.cpp, Process_win32.cpp, ProcessLauncher_test.cpp,
  testing/FakeProcessLauncher.hpp, testing/FakeProcessLauncher.cpp, testing/RecordingLauncher.hpp,
  testing/LaunchProbe.cpp (new)
- CMakeLists.txt, HostConnector.hpp (modified)

## TDD evidence
- RED: tests + headers written first, implementations as empty stubs; building `vtextension_process_test`
  failed to link — undefined `makeSystemProcessLauncher`, `ControlPage::{create,adopt,raise,clear,isRaised,…}`,
  `formatHelperArguments`, `parseHelperArguments`, `makeWakePair`, `WakeReceiver::{wait,stop}`,
  `WakeSender::signal`, … (21 distinct symbols). The two `Channel.adopt*` cases in `vtextension_test` were not
  separately run RED (that binary was only built after `adopt` existed).
- GREEN: all passed on the first full build.
- The tests bite. With `addclosefrom_np` and `addchdir_np` removed, `childRunsAtTheRoot` failed (cwd was the
  worktree). The first version of `childInheritsExactlyItsHandles` still passed without `closefrom`: the stray
  descriptor got number 3, and the dup2 onto 3 hid it. The test now parks the stray at ≥ 32, and with
  `closefrom` removed it fails (`{0..9, 32}` vs `{0..9}`). The launcher is restored.

## Test results (real counts)
- `vtextension_process_test` (clang-asan): 24 cases / 193 assertions, all pass; 8 full runs green.
- `vtextension_test "Channel.*"`: 8 cases / 36 assertions (the 6 earlier cases plus `adoptTakesAnInheritedEnd` and
  `adoptRefusesAClosedHandle`).
- `ctest --preset=clang-asan -R vtextension`: 4/4 pass (vtextension_test, vtextension_wasmtime_test,
  vtextension_process_test, vtextension_remote_test), run at HEAD.
- TSan (`out/clang-tsan`, only `vtextension_process_test` and `extension_host_probe` built): 24 cases / 193
  assertions. 3 runs, 0 ThreadSanitizer warnings.
- No child outlives a case. Every launcher case ends with `CHECK(endAndReap(...))` or an asserted `tryReap()`,
  and `Probe`'s destructor kills and reaps on a failure path. `ps` shows no leftover `extension_host_probe`
  after the runs.
- Windows (clang-cl cross, `out/clangcl-debug`, `-Werror`):
  - `vtextension_process_test.exe` and `extension_host_probe.exe` build clean.
  - Under wine 11.20 (own WINEPREFIX in the scratchpad), the debug CRT DLLs were copied from /opt/msvc into
    `out/clangcl-debug/bin`. Result: 20 cases / 180 assertions in 6 full runs, last one at HEAD.
  - The Windows-specific cases pass: `childIsInAKillOnCloseJob`, `killThenReap` with `KilledExitCode`, and the
    handle list keeping a stray inheritable event out.
  - `Channel_test.cpp` passes too: 9 cases / 42 assertions with `"Channel.*"`. For that run it was temporarily
    added to the cross target; the CMake edit was reverted.
  - `wineserver -k` was run on the own prefix only.
- clang-tidy, run by hand:
  `clang-tidy -p out/clang-asan --quiet <the 13 changed POSIX/portable .cpp files>`, 0 findings after fixes.
  The Windows sources are not in a compile DB that clang-tidy can use here, but the clang-cl `-Werror` build is
  clean.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` on every changed file: clean.

## Deviations from the brief, and why
1. **Windows wake event is manual-reset, not auto-reset (D47).** With auto-reset, `signal()` cannot tell a queued
   wake from a new one without consuming it, so `WakeResult::Pending` and `"WakeSignal.signalNeverBlocks"` (at least
   one `Pending`) would be impossible. With manual-reset, `signal()` returns `Pending` when the event is already
   set. The receiver resets the event after its wait and before its caller scans the page, so no raised flag is
   lost (reasoning in `WakeSignal_win32.cpp`).
2. **`WakeSender::signal()` and `WakeReceiver::stop()` are `const`.** clang-tidy's
   `readability-make-member-function-const` required it. Callers are unaffected.
3. **`PosixLaunch.{hpp,cpp}` added** (not in the file list). The three POSIX launchers share argv/envp building,
   the executable check, parking the inherited descriptors, errno/wait-status mapping, the spawn-object RAII
   (`SpawnObject` template), and the poll wait set for Linux and FreeBSD. Duplicating that three times was worse.
4. **Inherited descriptors are parked first.** Each is `F_DUPFD_CLOEXEC`'d to ≥ 3 + N in the parent, and the
   child `dup2`s those copies onto 3 + i. A direct `dup2` sequence clobbers a source that already sits at a target
   number. `closefrom` starts at 3 + N rather than a literal 10, which is the same 10 for the 7 handles a helper
   gets.
5. **The wait-set wake is a pipe on both Linux and FreeBSD**, not an eventfd on Linux (D40). This keeps one poll set
   for both. It behaves the same.
6. **FreeBSD uses `pdfork(PD_CLOEXEC)` as D36 says.** So destroying the handle (closing the descriptor) kills the
   child there, as the job's close does on Windows. `IProcessHandle`'s destructor doc says so. On Linux and
   macOS the destructor neither kills nor waits.
7. **The probe lists descriptors with `fcntl(F_GETFD)` over 0..min(OPEN_MAX, 4096)**, not by reading
   `/proc/self/fd` or `/dev/fd`. `readdir` trips clang-tidy `concurrency-mt-unsafe`, and the listing holds a
   descriptor of its own. It reads the environment from `main`'s third parameter: a redeclared `environ` is a
   clang-tidy finding on glibc.
8. **`MaxLaneChannels` moved** from `HostConnector.hpp` to `transport/HelperArguments.hpp`. Same namespace
   `vtextension::wasm::remote`. The helper's argument parser needs it, and it must not include
   `HostConnector.hpp` (which names `ExtensionError`).
9. **Tests added beyond the named list:**
   - `FakeProcessLauncher.recordsAndExits` and `RecordingLauncher.recordsAndRewritesLaterLaunches`, so the
     doubles have tests.
   - `RecordingLauncher` takes its optional argument rewrite at construction.
   - `FakeProcessLauncher`'s `kill()` ends a fake process with `FakeKilledStatus = {Signalled, 9}` and wakes its
     sets.
   - `kills()` returns the launch index of each `kill()` call.
   - `lastDeadline()` is the deadline of the latest `wait()`.
10. **Windows test adaptations.**
    - `childGetsACleanEnvironment` drops wine's own injected variables (`WINE*`, `SystemDrive`, `SystemRoot`,
      `PROCESSOR_ARCHITECTURE`), and only when running under wine (`wine_get_version` present). Real Windows
      stays exact.
    - The stray event in `childInheritsExactlyItsHandles` is created after 512 filler handles. Its value is then
      above any handle the child opens itself: at value 56 it collided with a handle of the child's own.
    - `childRunsAtTheRoot` compares with `absolute(probe).root_path()`.
11. **Other systems get no launcher target.** On a system other than Linux, FreeBSD, Darwin or Windows (OpenBSD,
    NetBSD), `vtextension_remote_process` and its tests are not defined. CMake does not fail.

## Not verified here
- **macOS (`Process_macos.cpp`, the shm branch of `ControlPage_posix.cpp`, `pipe` + `fcntl` in
  `WakeSignal_posix.cpp`) and FreeBSD (`Process_freebsd.cpp`)** were never compiled here: no SDK or sysroot. They
  are written carefully and guarded, and CI must verify them. Specific risks:
  - macOS `posix_spawn_file_actions_addchdir_np` may carry a deprecation attribute on the newest SDK, which
    `-Werror` would break.
  - The EVFILT_PROC-on-a-zombie path (ESRCH is treated as already exited).
  - FreeBSD `memfd_create` + `F_ADD_SEALS` needs FreeBSD ≥ 13.
- Linux needs glibc ≥ 2.34 for `posix_spawn_file_actions_addclosefrom_np` (Ubuntu 22.04 has 2.35).
- Kernel 5.3 exactly (pidfd_open but no `P_PIDFD` waitid) would report a lost status (-1) from `tryReap()`
  instead of `Unsupported`. That is out of practical reach, since D37's Landlock needs 5.13.

## Self-review notes
- **Risk.**
  - The launcher runs only from the supervisor thread (D42, a later task); nothing here enforces that.
  - `IExitWaitSet` is single-threaded except `wake()`. That is documented, and the TSan runs cover `wake()` from
    another thread.
  - `ControlPage` bounds-checks the slot. Out-of-range is a no-op or `false`, never a stray write into shared
    memory.
- **Performance.**
  - Nothing on a hot path yet. `signal()` is one non-blocking `send`, or `WaitForSingleObject(0)` +
  `SetEvent`.
- **Coverage.**
  - Not measured in this task (3.7 does).
  - Uncovered by tests: the error branches of launch (`Resources`/`Io` mapping, pidfd ENOSYS cleanup), the
    Windows wait set's > 63-process overflow path, and the macOS/FreeBSD files.

## Concerns
1. macOS and FreeBSD launcher/page/wake code is compiled only by CI. Expect possible fix-ups there.
2. The manual-reset event deviates from D47's wording. The controller should confirm.
3. The debug CRT DLLs were copied into `out/clangcl-debug/bin` to run under wine. That is a build-tree-only
   change, and Task 3.7 needs the same step.

## Fix round 1 (commit 1551a7793 `fix(vtextension): narrow what a helper process inherits and can write`)

1. **Windows inheritance window** (Process_win32.cpp):
   - The duplicates, the NUL handle and the stderr copy are now made non-inheritable.
   - The command line is built from their values.
   - `SetHandleInformation(HANDLE_FLAG_INHERIT)` runs right before `CreateProcessW()`, and all of them are
     closed right after it, whatever it returned.
   - The file head documents the window: QProcess or any list-less `CreateProcess` could inherit a lane end and
     break D59. It also notes that helper launches all run on the supervisor thread (D42), and points at
     macOS's own window.
2. **macOS attach race**: after a successful `EV_ADD`, `waitid(P_PID, WEXITED|WNOHANG|WNOWAIT)` runs once. An
   exit it finds is recorded and reported by the next `wait()`, de-duplicated against the event.
3. **HelperArguments**:
   - One handle in two roles → `Malformed`, and on POSIX a handle ≤ 2 → `Malformed`. The `ArgumentError::Malformed`
     doc says so.
   - Two table rows were added to `HelperArguments.rejects`: two roles on one handle, and (POSIX) `--page=2`.
4. **Read-only helper page**:
   - `ControlPage::adopt` maps `PROT_READ` (POSIX) or `FILE_MAP_READ` (Windows). `raise`/`clear` are documented
     as create()-side only.
   - Linux: `create()` seals SHRINK|GROW, maps RW, then adds `F_SEAL_FUTURE_WRITE` (EINVAL on a kernel older
     than 5.1 is tolerated: the fallback is the read-only mapping alone) and then `F_SEAL_SEAL`.
   - Windows: `create()` maps RW, then keeps only a `FILE_MAP_READ` duplicate as `nativeHandle()`; the view
     keeps the section alive.
   - FreeBSD/macOS: `PROT_READ` in the helper only, as documented.
   - New test `ControlPage.helperCannotWrite`:
     - Linux: `mmap(PROT_WRITE, MAP_SHARED)` of the page fails `EPERM`.
     - POSIX: a forked child adopts a duplicate and `raise(5)`s. It must not exit 0, and the parent must not see
       slot 5 raised. The child's stdout/stderr go to /dev/null and SIGSEGV/SIGBUS are reset to SIG_DFL, so
       neither Catch2's nor ASan's handler reports from it.
     - Windows: `MapViewOfFile(nativeHandle(), FILE_MAP_WRITE)` fails.
   - Mutation check: with adopt mapping RW and the FUTURE_WRITE seal removed, both the EPERM check and the
     forked-child check fail. Restored afterwards.
5. **FreeBSD**: `inheritAs(source, target)` clears `FD_CLOEXEC` when source == target instead of calling
   `dup2(n, n)`. Async-signal-safe.
6. **Windows SystemRoot** is built from `u8string()`.
7. **`IExitWaitSet::wait()` returns `std::expected<std::vector<uint64_t>, LaunchError>`.** This changes the
   interface the brief gave, which was the only way to "return an error the supervisor handles".
   - A failed wait returns `Io` (or the mapped errno) instead of an empty list, which a caller would retry and
     spin on.
   - The cases are Windows `WAIT_FAILED`, POSIX `POLLNVAL` (a watched descriptor closed while watched) or a
     failing `poll`, and a macOS failing `kevent`.
   - The doc tells the caller to stop waiting on that set.
8. **Tests**:
   - `WakeSignal.endsAreNotInherited` (FD_CLOEXEC / no HANDLE_FLAG_INHERIT on both ends).
   - The probe reports `stdin=`/`stdout=` as `null` (POSIX: same dev+ino as /dev/null; Windows: a character
     device with no console mode). The case `childRunsAtTheRoot` is renamed
     `childRunsAtTheRootOnTheNullDevice` and asserts both.
9. **`AttributeList::set`** returns `std::expected<void, LaunchError>`, chained with `and_then`.

The optional pidfd_spawn item was skipped, as instructed.

### Results
- `vtextension_process_test` (ASan): 26 cases / 210 assertions in 4 full runs.
- `ctest --preset=clang-asan -R vtextension`: 4/4.
- TSan: 26 / 210 in 3 runs, 0 warnings.
- clang-tidy on the 10 changed portable/POSIX .cpp files: 0 findings, after one fix (a `dup2(-1)` analyzer
  finding in the fork test).
- clang-format and editorconfig-checker: clean.
- Windows:
  - The clang-cl cross-build is clean.
  - Under wine (own prefix): 22 cases / 190 assertions. Exit 0 in 3 runs; one run's summary line was lost in
    wine's console output, but it still exited 0. `wineserver -k` was run on the own prefix only.
