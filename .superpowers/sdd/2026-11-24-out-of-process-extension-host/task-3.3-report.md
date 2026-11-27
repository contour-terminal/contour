# Task 3.3 report: ExtensionHostSupervisor, the process connector, non-call timeouts

Status: DONE_WITH_CONCERNS. The concerns: the macOS CMake is only parse-checked, TDD evidence is from mutations, and some decisions go beyond the brief (listed below).

## Commits (on c67eb8637)
- 4d471786c `build: bundle contour-extension-host whenever it is rebuilt` (item 0)
  - a. `src/contour/CMakeLists.txt`, bundle branch:
    - `add_custom_command(OUTPUT <bundle>/Contents/Helpers/contour-extension-host DEPENDS contour-extension-host $<TARGET_FILE:contour-extension-host>)`.
    - Wrapped in the custom target `contour_bundled_extension_host`, and `add_dependencies(contour contour_bundled_extension_host)`.
    - No helper→contour edge.
    - An OUTPUT may not use target-dependent genexes, so the bundle path is spelled `${CMAKE_CURRENT_BINARY_DIR}[/$<CONFIG>]/contour.app`. The `$<CONFIG>` part applies to multi-config generators.
    - Verified by:
      - Configuring the real tree on Linux; it parses.
      - Running the block verbatim in a mini project (Ninja and Ninja Multi-Config, in the scratchpad). The copy lands in `contour.app/Contents/Helpers`, and touching only the helper's source re-runs the bundling.
    - Not built on macOS.
  - b. `ProcessLauncher_test.cpp`: the copied descriptor is closed when `watch()` fails, before the REQUIRE.
- c3daea7c3 `feat(vtextension): supervise extension host processes, with timeouts on start, load and instantiate`

## Files
- New:
  - `wasm/remote/process/{ExtensionHostSupervisor,HostTimers,ProcessHostConnector}.{hpp,cpp}`
  - `ExtensionHostSupervisor_test.cpp`, `HostTimers_test.cpp`
  - `wasm/testing/ProcessModules.hpp` (`StartSpinModule`)
- Modified:
  - `HostConnector.hpp` (D61, header-only: `HostStep`, `StepWatch`, `HostAttachment`, `IHostHandle::watch/attach`)
  - `HelperIncarnation.{hpp,cpp}`, `RemoteBackend.cpp`, `BackendConfig.hpp` (`LockdownRequirement`)
  - `host/InProcessHost.{hpp,cpp}`, `remote/testing/RemoteDefaultBackend.cpp`
  - `RemoteBackend_test.cpp`, `RegistryRemote_test.cpp`
  - `ExtensionRegistry.{hpp,cpp}`, `ExtensionRegistry_test.cpp`, `SessionExtensions_test.cpp`
  - `testing/FakeWasmBackend.hpp`, `testing/RunnerHarness.hpp`, `Helper_test.cpp`
  - `process/ProcessLauncher.hpp` + `PosixLaunch.cpp` + `Process_win32.cpp` (`currentProcessId()`)
  - `process/testing/FakeProcessLauncher.{hpp,cpp}`, `process/testing/LaunchedHelper.cpp`
  - `remote/CMakeLists.txt`

## Design (what the signatures left open)
- **Loop.** Each iteration does the following, in order:
  1. `clock.refresh()`.
  2. Take the batch and process commands: Launch, Watch, Disarm, Attach, Kill, Drop, Sync, Stop.
  3. Reap every live helper by `tryReap()`. Polling all, not only the keys `wait()` returned, makes `sync()` cover exits that happened before it.
  4. `expire(now)`, then reap again.
  5. Publish the counters, then fulfil the Syncs.
  6. `wait(nextDeadline())`.
  - `onDeath` runs on the supervisor thread with no mailbox lock held.
- **Watches are timed from the caller's instant.** `SupervisedHelper::watch()` reads the clock under the mailbox lock while the mailbox is open; the clock outlives the supervisor, which closes the mailbox before it goes. The watch posts `Watch{timer, step, start, budget}`, and the returned StepWatch posts `Disarm`.
  - Serials come from one mailbox-wide atomic. Serial 0 is reserved for a dropped helper's teardown timer.
- **`HelperIncarnation::request(pdu, step)`** arms its watch only once it holds the control mutex. Waiting for another thread's exchange is not the host's time.
  - Hello and CreateRuntime share one Handshake watch.
- **Unclean exit:** anything other than `{Exited, 0}`, unless the supervisor sent a kill and the status is a kill's. A table holds both kill statuses: `{Signalled, 9}` and `{Exited, KilledExitCode}`.
  - A crash after a requested kill (signal 11) still counts as unclean; tested.
- **Drop:**
  - Forgets the attachment and the step timers.
  - Already reaped: erased.
  - Killed already: left to the reap.
  - `teardownGrace > 0`: a teardown timer is armed, and the helper is killed with reason `Dropped` on expiry unless it exits first.
  - Otherwise: killed at once.
- **R20 (failed wait).** Decision: the supervisor retires.
  - It logs once, kills every helper (`KillReason::Shutdown`), and runs every onDeath.
  - It refuses later launches with `LaunchError::Stopped`, which the connector maps to `HostUnavailable`.
  - It keeps processing commands, waiting on the mailbox's condition variable instead of the set, and polls reaps every 100 ms.
  - That 100 ms is the only real-time wait, and runs only on a broken system.
  - Tested by `Supervisor.failedWaitStopsEveryHelper`, using a new `FakeProcessLauncher::failWaits()`.
- **Stop:**
  1. With `teardownGrace` set, reap until the grace ends, by the injected clock.
  2. Kill every helper (`Shutdown`).
  3. Reap until `ShutdownReapBudget` passes, by the injected clock.
  4. Run onDeath for the rest, unwatch them, and destroy their handles.
  - Under a ManualClock the budget passes only when the test moves the clock: `stopIsBounded` advances the clock and nudges.
- **The `InProcessHost` attachment (D61):** `attach` stores `onDeath` and calls it once `run()` returns, or at once if it already returned.
  - The onDeath may drop the last incarnation reference on the host's own thread. `~InProcessHost` then detaches instead of self-joining, and the thread touches nothing of the host after onDeath.
- **D53:** the registry keeps `KeyBreaker{RegistryKey, shared_ptr<HostBreaker>}`.
  - `acquire` admits under the lock first: an open breaker returns `HostUnavailable` and loads nothing.
  - A `HostUnavailable` load records a fault with that admission.
  - `resetBreakers()` prunes keys without an entry and resets the rest.
- **Lane stalls (the phase-2 deferred note).**
  - Control-channel stalls, a valid header followed by nothing included, are bounded now: the step watch kills the helper and `markDead` shuts the channels.
  - Lane stalls, including a copy-path `MemoryData` that stalls after its header, are not bounded in this task. Task 3.4's backstop bounds them (D44/D49): the watchdog interrupts, the grace timer (`KillReason::Grace` exists, `HostTimers::pause/resume` are ready) kills, and the kill closes the lanes.
  - Until 3.4, `ProcessHostHandle::interrupt` is a documented no-op.

## TDD evidence
- **Honest note:** tests and implementation were written in the same pass, so there is no separate RED build. Instead, each core behaviour was mutated and its tests were shown to fail, then the source was restored:
  - Removing `expireTimers()` from the loop: `Supervisor.stepTimeoutsKill` fails (12 of 36 assertions).
  - Removing `recordHostFault` in the registry: "load timeout counts a breaker fault" and "reload resets a load breaker" fail (2 assertions).
  - Accepting Unsandboxed under RequireLockedDown, plus removing the request watch: the RemoteBackend cases hang, because the scripted host never answers CreateRuntime; killed by `timeout 300`. That is a failure, not a pass. `eachStepIsWatched` alone was not isolated under the watch mutation.
- **Two flakes found and fixed in my own tests.** Both were races between posting a command and moving the manual clock:
  - A Disarm racing `clock.advance`.
  - A Drop's grace counted from after an advance.
  - The fix: `sync()` before advancing. Both are documented in the cases.
  - After the fix: 30 consecutive runs of `"HostTimers.*,Supervisor.*"` were green.
- **A third race, found under wine.** `Helper_test` read the counters before the batch that killed the helper had published them. `killsFor()` now calls `sync()` first.

## Test results (real counts)
- ASan, `out/clang-asan`:
  - `vtextension_process_test`: 55 cases, 54 passed, 1 skipped (`capsHold`, as before). `"HostTimers.*,Supervisor.*"` is 24 cases / 148 assertions.
  - `vtextension_helper_test`: 7 cases, 6 passed, 1 skipped (caps). `"Process.*"` is 3 cases / 32 assertions, each ending with `liveProcesses()==0` and `uncleanExits==0`.
  - `vtextension_remote_test`: 200 cases / 4365 assertions.
  - `vtextension_test`: 395 cases / 13334 assertions. That includes the 4 new registry cases and "status-line load never runs on the owner thread".
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7 passed.
  - No leftover helper or probe processes (`ps` count 0).
- TSan, `out/clang-tsan`, built at -j4 under nice:
  - `vtextension_process_test`: 3 runs, 55 cases each, 0 ThreadSanitizer warnings.
  - `vtextension_remote_test "RemoteBackend.*,HostServer.*,HostProcess.*"`: 57 cases / 1373 assertions, 0 warnings.
  - `vtextension_helper_test`: 7 cases with the TSan-built real helper, 0 warnings, `uncleanExits == 0`.
- Windows, clang-cl cross (`out/clangcl-debug`, -Werror):
  - `vtextension_process_test.exe` and `vtextension_helper_test.exe` build clean.
  - Under wine, with my own prefix in the scratchpad: process test 48 cases / 351 assertions, all pass. Helper test 5 cases / 52 assertions, 3 runs green, including the real-helper `Process.*` cases (load+call, lockdown refusal, start-function timeout kill and respawn).
  - `wineserver -k` was run on my own prefix only.
- clang-tidy, by hand: `clang-tidy -p out/clang-asan --quiet <the 18 changed non-win32 .cpp files>`, then again on the 4 re-edited files. 0 findings after the fixes (unused usings, two const methods, one DeMorgan).
  - `Process_win32.cpp` is covered by the clang-cl -Werror build only.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` on every changed file: clean.

## Deviations, and why
1. **`core::log` is linked into `vtextension_remote_process`** (D30 lists base and platform), for `errorLog()`. It is still std-only, still cross-builds, and ran under wine.
2. **API additions beyond the brief:**
   - `HostTimers::disarmHelper()`: a reaped or dropped helper's timers go with it.
   - `KillReasonCount`.
   - `process::currentProcessId()`: POSIX in `PosixLaunch.cpp`, Windows in `Process_win32.cpp`. The testing `ownProcessId()` now forwards to it.
   - `FakeProcessLauncher::survivesKill()` and `failWaits()`.
   - `RunnerHarness(std::unique_ptr<IWasmBackend>)`, so that the SessionExtensions case runs on a `FakeWasmBackend`.
   - `SupervisorLoop` is a namespace-scope class, forward-declared, rather than a nested one: the mailbox befriends it.
3. **The D53 breaker is keyed by `RegistryKey` only, as D53 says.** Entries stay per engine, but two engines loading the same key now share one breaker. Before, each entry had its own.
4. **"Never the owner thread (asserted in 3.3)"** is not a runtime assertion. `SupervisorMailbox::launch` does `Require(thread != supervisor thread)`, which avoids a self-deadlock. The owner-thread property is covered by the SessionExtensions test, because the mailbox cannot know the owner thread.
5. **`deathsObserved` vs `reaped`:** both count on every reap while the loop runs. `reaped` also counts the reaps during stop.
6. **The SessionExtensions case runs builds and lanes on fresh `jthread`s.** The ManualExecutor and ManualScheduler would otherwise run them on the test (owner) thread.
7. **Extra cases beyond the brief:**
   - Supervisor: `watchIsTimedFromItsCaller`, `armedWatchIsWaitedFor`, `crashAfterAKillIsStillUnclean`, `dropWaitsTheTeardownGrace`, `helperExitingWithinTheGraceIsNotKilled`, `failedWaitStopsEveryHelper`, `moveHandsTheHelperOn`.
   - HostTimers: three extra cases.
   - RemoteBackend: `unsandboxedIsRefusedUnlessAccepted`, `eachStepIsWatched`, `observedDeathKillsTheIncarnation`.
   - Registry: `faultsWhileLoadingCarryOverToTheRunningEntry`.
8. **The "ten panes respawn one helper" case was not built.** The plan gives it to 3.4.

## Concerns
1. **macOS bundle CMake:** parse-checked on Linux and exercised in a mini project only.
2. **A watch-failed launch** (the set refuses `watch`) is killed and kept only to be reaped by polling. If no further wake comes, its zombie lingers until the next loop iteration. This is an edge case.
3. **Stop under a ManualClock with a helper that never exits** blocks until the test moves the clock. Real clocks are bounded. A failing fake test could hang rather than fail (seen once while developing).
4. **Performance:** nothing on a call's hot path. A Load or Instantiate adds two mailbox posts (watch and disarm) and wakes the supervisor twice. Not measured; 3.7 measures.
5. **Coverage:** not measured; 3.7 does. Uncovered:
   - `SupervisorError::ThreadUnavailable`
   - `WaitSetUnavailable`
   - the watch-failed launch branch

## Fix round 1

1. **(Important) `Supervisor.stopIsBounded` can no longer hang.** If the stop does not finish within 1 s of the loop, the case does three things:
   - calls `FAIL_CHECK`;
   - writes one line to stderr;
   - calls `std::abort()`.

   Waiting on the future's destructor would block forever, and the rig cannot go while the stop runs on it.
   - Mutation: `reapUntil` ignores its deadline. The case aborts with the message after 2.01 s, where before it would have hung.
2. **(Important) `loadAfterHello`'s scripted host now answers what it is asked.** It replies to CreateRuntime with RuntimeCreated, and to Load with a description, then drains.
   - A parent that wrongly accepts the host now ends with a loaded module, and the case fails at once.
   - The lockdown case's hand-written locked-down script is replaced by the same helper. It now checks four things:
     - Unsandboxed under RequireLockedDown fails `HostUnavailable`, with 1 termination.
     - Unsandboxed under AcceptUnsandboxed loads, with 0 terminations.
     - LockedDown under RequireLockedDown loads, with 0 terminations.
     - The version-mismatch and Failed-lockdown cases are unchanged, and they still pass.
   - Mutation: D52 accepts everything. `RemoteBackend.unsandboxedIsRefusedUnlessAccepted` fails in 16 ms, where before it hung.
3. **`SupervisorMailbox::now()` calls `_clock->refresh()` before `now()`.** The doc says why: an idle supervisor refreshes nothing, so a caching clock would be stale.
   - New case: `Supervisor.watchRefreshesTheClock`. A recording clock shows the caller's thread refreshed it.
   - Mutation: without the refresh, the case fails.
4. **`killProcess` returns early once `killed == Sent`.** A second kill is neither sent nor counted.
   - New case: `Supervisor.killIsCountedOnce`. A step timeout followed by the holder's `kill()` gives one kill, StepTimeout 1 and Requested 0.
   - Mutation: the case fails.
5. **A launch whose watch the set refuses:**
   - It counts as a launch.
   - It is killed as `KillReason::Shutdown`, whose doc already covers "can no longer wait for its helpers".
   - It is marked `Watching::Polled`.
   - `nextWake()` adds `now + DegradedPollPeriod` (by the injected clock) to the wait deadline while any polled helper lives.

   This keeps the counters consistent: `launches`, `liveProcesses` and `kills` all see it.
   - The fake gained `failWatches()`, and `survivesKill()` now also accepts a process launched later.
   - New case: `Supervisor.refusedWatchIsPolledAndReaped`, which checks the deadline, the reap and the deadline returning to none.
   - Mutation: without the poll deadline, the case fails.
6. **In the failed-wait state the supervisor waits without a timeout** once no helper is live and no timer is armed: `waitForCommands(std::optional<milliseconds>)`.
   - New case: `Supervisor.failedWaitIdlesOnceNothingIsLeft`. With the set broken and nothing left, no refresh happens within 350 ms of real time, where polling would have refreshed three times. It is one of the few real-time waits.
   - Mutation: polling always, the case fails in 0.36 s.
7. **`post()` returns `SupervisorMailbox::Delivery { Dropped, Queued }`**, marked `[[nodiscard]]`. Fire-and-forget callers write `std::ignore`.

All six mutations were applied in one build, each case was run alone under `timeout 60`, then the sources were restored.

### Results
- ASan:
  - `vtextension_process_test "HostTimers.*,Supervisor.*"`: 28 cases / 168 assertions. 30 consecutive runs green.
  - `vtextension_remote_test "RemoteBackend.*"`: 24 cases / 256 assertions.
  - `vtextension_helper_test`: 7 cases, 6 passed, 1 skipped.
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7.
- TSan: the supervisor tests 3 times (28 cases each) and `RemoteBackend.*` (24 cases), 0 warnings.
- Windows (wine, own prefix): `vtextension_process_test.exe` 52 cases / 371 assertions.
- `clang-tidy -p out/clang-asan --quiet` on the 4 changed .cpp files: 0 findings. clang-format and editorconfig-checker: clean.
