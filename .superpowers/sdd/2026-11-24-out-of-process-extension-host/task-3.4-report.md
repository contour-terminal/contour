# Task 3.4 report: interruption across the process boundary, and the backstop

Status: DONE_WITH_CONCERNS. The concerns are minor; they are listed at the end.

## Commits (on d00183270)
- 558666461 `docs(vtextension): name the supervisor mailbox's deliveries and where its polling is real time` (item 0)
  - `post()`'s `@return` names both `Delivery` values.
  - `Watching::Polled` says the poll period is by the injected clock only while the wait set works. Once a wait failed, the loop and `reapUntil()` both wait on the mailbox, and that period is real time.
- 98cb0f701 `fix(vtextension): reap the exited helper before moving the clock in the teardown-grace case`
  - Fixes a pre-existing flake in 3.3's test, found under wine (it failed 2 runs in 3).
  - `Supervisor.helperExitingWithinTheGraceIsNotKilled` moved the manual clock without a `sync()` after `fake->exit()`.
  - A loop turn already under way, woken by a wake left over from the batch before, could reap before the exit and expire timers after the clock moved.
  - After the fix: 0 failures in 8 full wine runs.
- a978e239b `feat(vtextension): interrupt an extension host without waiting on it, and kill one that ignores it`

## What changed (paths under src/)
- **Wire.** `proto/Pdu.hpp` has `ProtocolVersion = 2` and `Instantiate.interruptSlot`. Changed alongside: `Codec.cpp` and `Codec_test.cpp` (the sample's slot is 1023).
- **Lane watch.** New `remote/LaneWatch.{hpp,cpp,_test.cpp}`, namespace `remote`, compiled into `vtextension_remote_process`.
  - It uses atomics only.
  - Each recorded frame (`MaxCallDepth + 1` of them) has a serial that is unique on its lane, plus the instance id, a `Running`/`ServingHost` state and an `armedSerial`.
  - `interrupt(id)` arms the outermost frame of `id` by storing that frame's serial into its `armedSerial`. It is a scan plus one store: no CAS loop, wait-free.
  - An arming counts only while `armedSerial == serial`. So it ends when the frame ends, and a frame entered later in the same place is never armed by it. A race with leave/enter can only ever produce an arming that is never seen.
  - `LaneObservation.epoch` is the serial of the outermost armed frame, and 0 while disarmed.
  - Deeper frames are counted but not recorded; they report `InHelper`.
- **Connector vocabulary** (`HostConnector.hpp`):
  - `HostAttachment` gained `lanes` (`shared_ptr<LaneWatchSet const>`) and `interruptGrace`.
  - `IHostHandle::interrupt(instanceId, interruptSlot)` and `clearInterrupt(slot)`.
- **Parent side.**
  - `HelperIncarnation` owns `InterruptSlots` and a `shared_ptr<LaneWatchSet>`, and attaches both with `policy.timeouts.interruptGrace`.
    - `interrupt(id, slot)` arms the lanes first, then calls `handle->interrupt`.
    - `InterruptSlots` is a 1024-entry state array behind a mutex. It hands slots out round-robin, so a released slot comes back last.
  - `RemoteBackend.cpp`:
    - `instantiate` acquires a slot. With none free it returns `RuntimeUnavailable` without `markDead`, and it releases the slot on every failure path.
    - `~RemoteInstance` runs `clearInterrupt` → `Release` → slot release, in that order.
    - `call()` enters a `LaneFrame` before the flush and checks the sticky flag again after entering. Together with `interrupt()`'s order this is a Dekker pattern, all seq_cst.
    - `fetch()` is a frame of its own.
    - `serveHostCall` parks the frame (`HostCallFrame`) around `_context.callHost()` only. The flush and the `HostResult` send stay on the host's time, so a helper that stops reading is still bounded.
  - `RemoteKindState::lanesInUse()` is a new test hook.
  - `ProcessHostHandle`: `interrupt` = `page.raise` + `wake.signal` + `helper.nudge`; `clearInterrupt` = `page.clear`.
- **Supervisor** (`ExtensionHostSupervisor.cpp`):
  - Each `Supervised` keeps the attachment's lanes, its grace and a per-lane `LaneTrack`.
  - A second `HostTimers` (`_graceTimers`, keyed by `{helper, lane index}`) holds the graces, so they never collide with step serials.
  - On each loop turn, `observeLanes()` runs between the first reap and `expireTimers()`. For each lane:
    - newly armed, or the epoch moved → `arm`;
    - disarmed → `disarm`;
    - `ServingHost` ↔ other → `pause`/`resume`.
  - While any lane is armed, the wait deadline is at most `now + max(grace/10, 1ms)`.
  - On expiry: `KillReason::Grace`, the lanes are forgotten, `onDeath`, and an error log line.
  - Lanes are forgotten on drop, on reap and once a kill was sent.
  - `BackendConfig.hpp`'s doc no longer says the grace is unenforced.
- **Helper side.**
  - New `host/InterruptListener.{hpp,cpp}`: an `ExecutorThread` running `wait()` → on `Woken`, scan all 1024 slots; on `Stopped`/`PeerGone`, return. Its destructor calls `stop()` on the receiver and joins.
  - `HostProcess::run()` starts the listener before the server (so before `confine`, D37). If it cannot, `run()` returns `ExecutorUnavailable`.
    - It also sets `HostServerConfig::isInterruptRaised` from the page.
  - `HostServer`:
    - Replaced `interrupt(instanceId)` with `interruptSlot(slot)`.
    - Keeps `_slotInstances` (slot → instance) under `_mutex`. It is bound when the instance goes live, and unbound on Release, on a refused answer and in `release()`.
    - An out-of-range slot or a slot in use → `ProtocolViolation`.
    - `interruptSlot` re-reads the page under the lock (`isInterruptRaised`). Without that, a slot the listener read as raised for a released instance would interrupt the instance holding the slot now.
  - `InProcessHost` is now a `HostProcess` over in-memory endpoints.
    - The connector creates the page, a second read-only mapping (new `ControlPage::openHelperView()`, POSIX and win32) and the wake pair.
    - `interrupt` raises the slot and signals the wake, exactly as a process does (D48).
- **Hostile helper.** New `contour-extension-host/testing/{HostileHost.cpp,HostileModes.hpp}`, target `contour-extension-host-hostile`. It is built under `VTEXTENSION_TESTING` and never installed.
  - Table `HostileModes` currently has one row, `ignore-interrupt`.
  - `--hostile=<name>` is stripped from the command line.
  - `ignore-interrupt` re-formats the parsed arguments with `--wake` pointing at a wake pair of its own that nobody signals.
- **Helper test binary.**
  - New `remote/process/testing/HelperComposition.{hpp,cpp}`: a supervisor over `RecordingLauncher(system)` with a 2 s teardown grace, the sanitizer environment, `backend(config)`, `killsFor`, and `awaitEveryHelperReaped`.
    - `Helper_test.cpp` now uses it (its own copy was removed).
    - It has a per-case constructor and `binaryComposition()`.
  - New `remote/testing/ProcessDefaultBackend.cpp`, `remote/ProcessBackend_test.cpp` (`CONTOUR_WASM_BACKEND_CONTRACT(ProcessTraits)`) and `remote/ReviewFocusProcess_test.cpp`.
  - `vtextension_helper_test` gains the six real-guest suites (as `remote_test` in 2.5), the guest dependencies and `VTEXTENSION_TEST_MODULE_DIR`.
  - Its CTest command passes `~[in-process]`.
- **CMake.**
  - `vtextension_remote_process` now always exists and holds `LaneWatch`.
  - The launcher, supervisor and connector are added to it only where a platform source exists.
  - `vtextension_remote` links it.
  - The helper test's guard became `_process_source`.

## Tests (names as the brief asks)
- **LaneWatch:**
  - `interruptArmsOnlyAFrameOnTheLane`, `leavingTheInterruptedFrameDisarms`, `hostCallParksTheInnermostFrame`, `deepNestingIsBalanced`, `LaneWatchSet.interruptRacesFrames` `[threading]`.
  - Extra: `frameEnteredAfterItsInstanceLeftIsNotArmed`, `LaneWatchSet.interruptsTheLaneTheInstanceRunsOn`.
- **Supervisor:**
  - `graceKillsAfterHelperSideTime`, `graceIsPausedWhileServingCallHost`, `graceEndsWithTheCall`, `noGraceWithoutAFrame`, `armedLaneTicksAtATenthOfTheGrace`.
  - Extra: `graceRestartsForAnotherFrame`, `graceIsForgottenWithTheHelper`.
  - Each lane change is followed by `sync()` before the clock moves. The supervisor timestamps only when it looks, which a manual clock never triggers by itself.
- **HostServer:**
  - `interruptSlotInterruptsItsInstance` (replaces `interruptStopsOneInstance`), `slotOutOfRangeIsAViolation`, `slotInUseIsAViolation`.
  - Extra: `releasedSlotIsHandedOn`, `slotLoweredSinceIsNotInterrupted`.
  - `InProcessHost.handleInterrupts` now goes through the page and the wake.
  - The scripted parent's `instantiate(m, i)` uses slot `i` by default.
- **HostProcess:** `raisedFlagAndWakeInterrupt`, `wakeWithoutAFlagInterruptsNothing`.
- **ControlPage:** `helperViewSeesTheRaise`.
- **RemoteBackend:**
  - `interruptIsWaitFreeOfTheHelper` is unchanged and passes. Also `slotsAreReused` (2000 sequential) and `slotExhaustionIsRuntimeUnavailable` (1024 live, the 1025th refused, the first still calls, `connects() == 1`).
  - Extra: `releasedSlotIsLowered`. It interrupts and drops an instance, waits until its slot comes round again, wakes the listener via another spinning instance, and checks the heir is not interrupted.
- **Review Focus** (exact names): `"helper death during CallHost frees the lane"`, `"interrupt never blocks on a full pipe"` (POSIX), `"helper ignoring interrupt is killed after grace"`, `"ten panes respawn one helper"`.
- **No hang on regression.** Every wait is bounded:
  - `within()`/`abandon()` (FAIL plus `abort()`) wherever a future's or a thread's destructor would otherwise wait.
  - Bounded lane receives in `HostServer_test` (`receiveInterrupted` stops the server on timeout) and in `HostProcess_test` (`ServingHost::receiveOnLane` shuts the control channel on timeout; a `ConnectionEnd` member shuts it before the host thread joins, so a failed REQUIRE cannot hang).
  - The latched host context waits at most 10 s.

## TDD evidence
- **LaneWatch: real RED.** Against a stub `.cpp`, 6 of 7 cases failed (22 assertions). With the implementation: 7 of 7.
- **Elsewhere: compile-RED plus mutations.** The tests did not compile against the old API (Step 5). Each mutation was applied, the binary built, the cases run, and the source restored:
  - **M1.** No `observeLanes()`: 4 grace cases fail. The other four expect no kill.
    - Through the real hostile helper, `"helper ignoring interrupt is killed after grace"` fails bounded in 5.1 s ("the helper ignoring the interrupt was not stopped"), rather than hanging.
  - **M2.** No pause on `ServingHost`: `graceIsPausedWhileServingCallHost` fails.
  - **M3.** No page re-check in `interruptSlot`: `slotLoweredSinceIsNotInterrupted` fails.
  - **M5.** No slot validation: `slotInUseIsAViolation` and `slotOutOfRangeIsAViolation` fail.
  - **M6.** No `clearInterrupt` in `~RemoteInstance`: `releasedSlotIsLowered` fails.
  - **M4.** The listener drops slots: `raisedFlagAndWakeInterrupt` and `InProcessHost.handleInterrupts` fail bounded (10 s total).

## Results (real counts)
- **ASan** (`out/clang-asan`):
  - `vtextension_process_test`: 74 cases, 73 passed, 1 skipped (caps); 500 assertions. `"Supervisor.*,LaneWatch*"`: 35 cases / 222 assertions, 10 consecutive runs green.
  - `vtextension_remote_test`: 209 cases / 6604 assertions.
  - `vtextension_helper_test "~[in-process]"`: 141 cases, 140 passed, 1 skipped (caps); 2244 assertions; 3 runs green, about 8 s each.
    - The contract alone (`[wasm-contract]`) is 41 cases / 482 assertions; the brief said 40, and two of the 41 are POSIX-only.
    - `[process]`: 4 cases / 97 assertions.
  - `vtextension_test`: 395 cases / 13040 assertions.
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7 passed, after the final edits.
- **TSan** (`out/clang-tsan`; built: `vtextension_process_test`, `vtextension_remote_test`, `contour-extension-host`, the hostile helper, `vtextension_helper_test`):
  - `vtextension_process_test`: 3 runs, 0 ThreadSanitizer warnings. `LaneWatch*,Supervisor.*` is 35 cases.
  - `vtextension_remote_test "RemoteBackend.*,HostServer.*,HostProcess.*,InProcessHost.*"`: 70 cases / 3639 assertions, 0 warnings.
  - `vtextension_helper_test "~[in-process]"`, with TSan-built real helpers whose stderr is inherited: 2 runs, 141 cases each, 0 warnings, `uncleanExits` as asserted.
- **Windows** (clang-cl cross, `out/clangcl-debug`, -Werror):
  - These build clean: `vtextension_process_test`, `extension_host_probe`, `contour-extension-host`, `contour-extension-host-hostile`, `vtextension_helper_test`.
  - Under wine 11.20, with my own prefix in the scratchpad:
    - `vtextension_process_test.exe`: 67 cases / 456 assertions; 8 runs green after the flake fix.
    - `vtextension_helper_test.exe "~[in-process]"`: 114 cases, 112 passed, 2 skipped; 1460 assertions; 3 runs green.
      - It includes the contract through the real Windows helper and the three non-POSIX Review Focus cases (`[process]`: 3 cases / 88 assertions).
      - The guest suites now pull in `zlib1.dll`, which the cross-deps tree does not ship. For the run I put mingw's `zlib1.dll` in the scratchpad on `WINEPATH`; nothing was copied into the build tree.
  - `wineserver -k` was run on my own prefix only.
- **clang-tidy**, by hand: `clang-tidy -p out/clang-asan --quiet <the 26 changed or new non-win32 .cpp files>`.
  - First pass: 4 findings. Two `modernize-use-std-print` became `std::println`. A use-after-move in a `REQUIRE` was fixed by hoisting the send.
  - Re-run on the fixed files: 0.
  - The `_win32` sources are covered by the clang-cl -Werror build.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` on every changed file: clean.

## Deviations, and why
1. **`uncleanExits` in the two cases that kill from outside** (ruling R23 confirms this). "helper death during CallHost" and "ten panes" assert `uncleanExits == 1`, not 0. A SIGKILL the supervisor did not send is unclean by D57's definition. The case kills exactly one helper, so 1 means "that one, and no other". The other two cases assert 0.
2. **Removed API.** `HostServer::interrupt(instanceId)` is gone (D48 replaces it). Its test became the slot test.
3. **Additions beyond the brief:**
   - `HostServerConfig::isInterruptRaised`: the slot-reuse race fix above.
   - `ControlPage::openHelperView()`: an in-process host needs a second, read-only mapping, and `testing::duplicate` is test-only.
   - `HelperIncarnation::laneWatch()`, `interruptSlots()` and `clearInterrupt()`.
   - `LaneFrameCapacity` and `InterruptListenerStackBytes`.
   - `MinimumLaneLookPeriod` (1 ms): keeps a grace under 10 ms from spinning the loop.
4. **`InProcessHost` page and wake.** They are created by `InProcessHostConnector::connect()` and passed into `InProcessHost`, rather than created by the host itself, so that a failure is `HostUnavailable` and never a half-built host.
5. **`HelperComposition` is shared.** It serves `Helper_test`, `ProcessDefaultBackend` and the Review Focus cases, rather than three copies. Each Review Focus case builds its own, so its counts (`launches() == 2`, `kills[Grace]`) are its own.
6. **`[in-process]`: `Instance.hostile.print`.**
   - The helper inherits stderr (D39) and, in sanitizer builds, writes "resource caps are off".
   - The case captures stderr, so it catches the helper's line.
   - The reason is recorded in the plan's "Phase-3 facts"; that file is local and not committed.
   - The contract's `wasiStdioGoesNowhere` passes through the real helper.
7. **"ten panes".** The case waits for `deathsObserved == 1` before rendering, for determinism; the brief left that implicit.
8. **Grace start.** The grace starts when the supervisor first observes the arming, which is within one nudge-triggered turn. This is D49's "timestamps with its own clock".

## R7, R14, R19
- **R7.** The backstop arms on the parent's side, from the parent's own frame record, whatever the helper's engine does. A lost wake-up in the helper's epoch callback therefore ends in a grace kill (`KillReason::Grace` → `HostCrashed`), not a hang. This is proven against a helper that never reads the page at all (`ignore-interrupt`).
  - In-process hosts get no grace: `InProcessHost::attach` ignores the lanes, and the in-process host is test-only.
- **R14.** Unchanged (3.3's Instantiate watch). The `StartSpinModule` case still passes through the real helper.
- **R19.** On Windows, `wait()` resets the manual-reset event before it returns `Woken`, and the listener scans afterwards. A raise after the reset comes with a `SetEvent` of its own.

## Concerns
1. **Pause precision.**
   - The supervisor samples a lane's place every grace/10 (5 ms). A helper whose host calls happen to coincide with every sample could have its own time counted as the parent's.
   - This is contrived: it needs aligned timing.
   - The opposite error is conservative: helper time is charged up to one sample late.
2. **Grace restarts.** Interrupting an outer frame while an inner one is armed restarts the lane's grace once per newly armed outer frame, at most `MaxCallDepth + 1` times.
3. **The [in-process] case above** should come back with Task 5.2's diagnostics descriptor.
4. **Windows run dependency.** Running `vtextension_helper_test.exe` under wine now needs a `zlib1.dll` on `WINEPATH`, because the guest suites link libpng. The same was true for `vtextension_remote_test.exe` in 3.2. Task 3.7's Windows step should know this.
5. **Not measured.** Performance and coverage are 3.7's.
   - The parent's added hot-path cost is a few seq_cst stores per call and fetch, plus one per host call.
   - `interrupt()` adds one `send(MSG_DONTWAIT)` and one eventfd write.

## Fix round 1

### Commits (on a978e239b)
- 21587f174 `fix(vtextension): never arm another instance's frame, nor lower a newer arming` (item 1)
- cec4fc1ba `fix(vtextension): bound reads and host calls of an interrupted instance` (items 2 and 4)
- 3e3cd46bd `test(vtextension): fail a run whose helper ended uncleanly, and keep helper output out of captures` (items 3 and 5)

### Items
1. **(Important) Wrong-frame arming.**
   - `LaneWatch::interrupt()` now reads the frame's serial first, then its instance. `enter()` stores the serial last, so the instance read is that frame's or a later one's. A race can now only arm a serial no frame has, never another instance's frame.
   - The arming is raised with a CAS loop and never lowered, so a stale interrupter cannot replace a newer arming.
   - New test hook `LaneWatchProbe` (a constructor parameter: a function pointer plus context), called between the two reads.
   - New tests:
     - `LaneWatch.interruptNeverArmsAnotherInstancesFrame`: the lane leaves 7 and enters 9 between the reads.
     - `LaneWatch.staleInterruptKeepsANewerArming`: the lane re-enters 7 and a second interrupt arms the new frame between the reads.
   - Mutations:
     - The old read order: the first case fails.
     - A plain store instead of the CAS: the second case fails.
2. **(Important) Fetch after an interrupt.**
   - `fetch()` re-checks the sticky flag after entering its `LaneFrame`. If set, it calls `HelperIncarnation::interrupt()` again, which arms this frame, raises the slot and wakes the backstop.
   - I chose re-arming over failing because `MemoryFailure` has no interrupted value. A helper that answers still serves the read; a helper that stalls is bounded by the grace.
   - New test: `RemoteBackend.readAfterAnInterruptIsArmed`.
     - Setup: a scripted host that reads none of its lanes. The test interrupts, then a read blocks.
     - It waits, bounded at 5 s, until a lane observes `Armed`, then plays the backstop's kill (`attachment.onDeath()`).
     - Result: the read ends `HostCrashed`.
   - Mutation: without the re-check, the case fails (5.0 s, bounded).
3. **(Important) D57 for the binary's helpers.**
   - A Catch2 listener, `UncleanHelperExits`, lives in `testing/ProcessDefaultBackend.cpp`. After each case it waits (bounded: teardown grace + 3 s) until the live helpers are back to the case's starting count. It records a failure if a helper outlived the case, or if `uncleanExits` grew.
   - Listeners cannot assert, so failures go into `vtextension::testing::runGateFailures()` (new `vtextension/testing/RunGate.hpp`). The shared `vtextension/test_main.cpp` prints each as `run gate: <case>: ...` and returns 1.
   - `HelperComposition::awaitLiveHelpersAtMost(n)` was added; `awaitEveryHelperReaped()` uses it.
   - Evidence (mutation): the helper leaked an unreachable 64-byte allocation in `runExtensionHost`.
     - `WasmContract.callsExport*` and `WasmContract.loadRejectsNonModule*` printed the LeakSanitizer reports.
     - Both cases still "passed".
     - The run exited 1 with `run gate: WasmContract.callsExport - ProcessTraits: 1 extension host(s) ended uncleanly` (and the same for the other).
     - Before this round the same run exited 0.
     - A first attempt with a pointer left in a register was not reported by LSan (reachable); the unreachable version was.
4. **(Minor) Host calls on an interrupted instance.**
   - `serveHostCall` checks the sticky flag first. If set, it answers `HostResult{ DeadlineExceeded }` without running the host function.
   - A call whose reply fails while the instance is interrupted reports `Interrupted`.
   - New test: `RemoteBackend.interruptedInstancesHostCallIsRefused`.
     - Setup: a scripted host receives the Call, waits until the test has interrupted, then sends CallHost.
     - Checks: the answer is `DeadlineExceeded`, the call fails `Interrupted`, and the host context recorded no call.
   - Mutation: without the refusal, the case fails (bounded).
5. **(Minor) Helper stderr.**
   - New `LaunchSpec::standardError`: an optional handle the child gets as stderr; nothing keeps this process's own.
     - Linux and macOS: an `adddup2` file action.
     - FreeBSD: the child's `dup2`.
     - All POSIX launchers park the descriptor like the inherited ones.
     - Windows: `copyOf(spec handle)` in place of `GetStdHandle`.
   - `ProcessHostConfig::standardError` passes it through.
   - `HelperComposition` takes a copy of fd 2 at static initialisation, before any case captured it, and hands it to every helper. Sanitizer output stays visible (D39) and out of captures.
   - Removed: `Instance.hostile.print`'s `[in-process]` tag and its Phase-3 facts entry. The case now passes through the real helper.
   - Mutation: without the Linux `adddup2`, the case fails, capturing `contour-extension-host: resource caps are off in this build`.
   - The `~[in-process]` filter stays in CTest, now matching nothing.
   - To satisfy `cppcoreguidelines-pro-type-member-init`, every `LaunchSpec` field now has a `{}` initializer.
6. **(Note)** See the risk summary below.

### Risk summary (addition)
- A lane's grace restarts each time a frame further out is newly armed. It cannot restart more than `MaxCallDepth + 1` times (9), which bounds the backstop at 9 × grace in the worst case.
- Ruling R23: in the two cases that kill a helper from outside, `uncleanExits == 1` is correct.

### Results (real counts)
- **ASan:**
  - `vtextension_process_test`: 76 cases, 75 passed, 1 skipped; 506 assertions. `LaneWatch*` is 9 cases.
  - `vtextension_remote_test`: 211 cases / 6621 assertions.
  - `vtextension_helper_test "~[in-process]"`: 142 cases, 141 passed, 1 skipped; 2256 assertions; exit 0, so no run-gate failure.
  - `vtextension_test`: 395 cases / 13040 assertions.
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7, after the final edits.
- **TSan:**
  - `vtextension_process_test`: 3 runs × 76 cases, 0 warnings.
  - `vtextension_remote_test "RemoteBackend.*,HostServer.*,HostProcess.*,InProcessHost.*"`: 72 cases / 3656 assertions, 0 warnings.
  - `vtextension_helper_test "~[in-process]"`: 2 runs × 142 cases, 0 warnings.
- **Windows** (clang-cl -Werror cross-build, wine, own prefix, `zlib1.dll` on `WINEPATH` as before):
  - `vtextension_process_test.exe`: 69 cases / 462 assertions; green in 3 of the 4 runs made after the fix.
    - One run failed `Supervisor.failedWaitStopsEveryHelper` line 395 (`reaped == 1`). This is a 3.3 case this round does not touch.
    - It did not reproduce in 10 runs of `"Supervisor.*"`. Reported, not fixed: a likely ordering race between the failed-wait kill and the polled reap under wine's scheduling.
  - `vtextension_helper_test.exe "~[in-process]"`: 114 cases, 112 passed, 2 skipped, in 3 runs; no run-gate failure.
- **clang-tidy** (`clang-tidy -p out/clang-asan --quiet <changed .cpp>`): 0 findings in the files that compile on Linux, after two fixes:
  - `LaunchSpec` member initialisers;
  - `originalStandardError` naming.
  - `Process_freebsd.cpp` and `Process_macos.cpp` cannot be parsed on Linux (missing system headers); CI compiles them.
- clang-format and editorconfig-checker: clean.

### Concerns
- **The stderr change touches every launcher.**
  - FreeBSD and macOS are compiled only by CI.
  - Production passes no `standardError`, so behaviour there is unchanged.
- **The run gate fails the binary, not the case.** A Catch2 listener cannot fail a case. The gate names the case on stderr and makes the binary exit 1, which CTest reports.
- **The wine-only flake** in `Supervisor.failedWaitStopsEveryHelper` (see above).
