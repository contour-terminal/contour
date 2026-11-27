# Task 3.6 report: crash and hostility suite

Status: DONE. The suite found no defect in the parent. Concerns are listed at the end.

## Commits (on 078b5be04)

- 6e7a04e77 `test(vtextension): assert nothing from a destructor, and sync past a failed wait` (item 0)
- d9a624925 `test(vtextension): crash and hostility suite for the extension host process`

## Item 0

- **a. Hook in a destructor.** In `RemoteBackend.releaseQueuedWhileARequestEndsIsSent`, the disarm hook (run from the `StepWatch` destructor) no longer asserts anything.
  - It records `Drop::Returned` in a local `enum class Drop { NotRun, Returned }`.
  - The case runs `CHECK(drop == Drop::Returned)` after `instantiate` returns.
  - A drop that does not return within 5 s still prints and calls `abort()`, so a regression cannot hang in the future's destructor. `abort` does not throw.
- **b.** The `HelperIncarnation::release()` doc comment is reflowed. Its premature line break was at ~126.
- **c. Wine flake `Supervisor.failedWaitStopsEveryHelper`. Reproduced and fixed.**
  - **Before the fix:** 7 of 30 single-case runs under wine failed, each at `killsFor(Shutdown) == 1` (0 == 1). The 3.4 report's "line 395 (`reaped == 1`)" was the same `killsFor` line in that version.
    - The first 30-run attempt hung, and the hang was in my harness, not in the test. Capturing wine output with `$(...)` waits for EOF on a pipe that a persistent `wineserver -p` service (winedevice, etc.) inherits. All later runs log to files with `</dev/null`.
  - **Cause (test timing, not wine semantics):** a failed wait is not a mailbox command, so a single `sync()` after `failWaits()` is not ordered after it.
    - The previous `sync()`'s post leaves the fake wait set's `_isWoken` flag set.
    - That flag can start one extra loop turn, whose `take()` picks up the next `sync()` before any wait has failed.
    - The counters published at that point have no kill yet.
  - **Fix:** `rig.settle()` (two syncs), with a comment explaining why.
    - The flag gives at most one spurious turn. Once `failWaits` is set, every wait fails. So the second sync is always taken by the turn after the failed wait, which kills, reaps and publishes.
    - Code unchanged: the supervisor behaves correctly.
  - **After the fix:** 0 of 30 under wine (cross-built clang-cl), and 0 of 100 on Linux ASan.

## What changed in the suite (paths under `src/`)

- **`contour-extension-host/testing/HostileModes.hpp`.** The `HostileMode` enum is gone. Each row is now `HostileModeRow { name, wake (Contour|Silent), afterEnd (Exit|SleepUntilKilled), optional<HostileIncident> }`.
  - `HostileIncident = { HostileTrigger { direction, channel kind, PduKind, optional after }, HostileAction }`.
  - There are 14 rows: the 11 new ones, plus `ignore-interrupt`, `ignore-eof` and `stop-reading-after-instantiate`. The last of these is now an incident row: `Load` once `Instantiated` has passed → `StopReadingChannel`.
  - `hostileModeNamed()` returns a row pointer.
  - Adding a behaviour means adding a row. Only a new `HostileAction` needs code.
- **`contour-extension-host/testing/HostileHost.cpp`.** `main` looks up the row, and `misbehave(row)` applies the wake source, the relay (only if there is an incident) and `afterEnd`.
  - The relay covers the control channel and all 4 lanes. It runs one pump thread per channel and direction (10 in total).
  - Each pump reads frame headers with `proto::frameSizeOf`, so a 16 MiB Load is streamed, not buffered, and forwards the raw bytes. A frame that fails to decode is passed on as it is, and the receiver refuses it.
  - The trigger fires at the header, once per process (atomic exchange). The "after" condition is a bitmask of the kinds that have passed.
  - The action returns a verdict: Forward, Discard, Stop or Hold. Hold means read and pass on nothing, forever.
  - Actions only complete or decode the frame when they need its content: `MemoryRead.size` for the short and half answers, and the full frame for ForwardThenExit.
  - Answers go back on the source channel, and a per-channel mutex serializes them with the forwarded frames.
  - Segfault: POSIX sets `SIG_DFL` then calls `raise(SIGSEGV)`. Windows calls `TerminateProcess(self, STATUS_ACCESS_VIOLATION)`.
  - Abort: on Windows it first calls `_set_abort_behavior(0, ...)`, so no dialog appears.
  - exit-in-callhost forwards the frame, then calls `std::_Exit(3)`.
  - over-cap sends a header with length `1 + maxMemoryBytes + 17`. It passes the generic largest-frame check and is refused by MemoryData's own cap.
  - **Without `--hostile` it now serves as the real host.** It used to refuse with exit 76. This is what makes the respawn benign.
- **`process/testing/HelperComposition.{hpp,cpp}`.** The constructor takes an `ArgumentRewrite laterArguments` and passes it to `RecordingLauncher`, which already had the rewrite. So `RecordingLauncher.hpp` needed no change.
- **`remote/Hostility_test.cpp`** (in `vtextension_helper_test`; listed in `remote/CMakeLists.txt`):
  - `Hostility.modes` takes one expectations table (`HostileRow { mode, Operation, Failure, HelperEnd }`) and runs one `DYNAMIC_SECTION` per row, as the brief specifies.
  - Each section composes:
    - a fresh `HelperComposition` with a teardown grace of 2 s and the `--hostile` drop;
    - timeouts of 200/200/200/50 ms and `AcceptUnsandboxed`;
    - a `Watchdog` jthread that interrupts the instance 50 ms after a call or read starts (it is a `condition_variable_any` with a `stop_token`).
  - Each section asserts:
    - the expected failure (a `Failure` enum mapped from `CallFailure` / `MemoryFailure` / `ExtensionError`);
    - elapsed time under 2 s;
    - `deathsObserved == 1` (awaited, bounded) and `launches() == 1`;
    - `uncleanExits` is 1 for abort, segfault and exit-in-callhost, and 0 otherwise;
    - recovery: the same module, or for the load rows a new load, gives `add(2,3) == 5` with `launches() == 2`;
    - `awaitEveryHelperReaped()`, and `uncleanExits` unchanged at the end.
  - Breaker cases, through `ExtensionRegistry`, `Engine` and `Instance` with `MinimalModule` (statusline), in a `HostileExtension` fixture:
    - `oneCrashIsOneFault`, `oneHangIsOneFault`, `oneViolationIsOneFault`;
    - `repeatedCrashesOpenTheBreaker` (`faultCount` 1, 2, 3, then Open; the 4th create is `HostUnavailable` with `launches == 3`);
    - `hangingLoadOpensTheBreaker` (3 acquires time out, and the 4th fails with `launches == 3`).
  - Every case is bounded:
    - a `CaseDeadline` jthread aborts the run after 60 s;
    - `awaitDeaths` and the reap waits are bounded;
    - the calls run on `contract::onPreparedThread`, under the case deadline.

## Step 5 (fixes in the parent)

None was needed.
- `bogus-size` was already refused, by `RemoteInstance::fetch`'s `bytes.size() != size` check.
- **Mutation check:** I dropped that check and rebuilt. The `bogus-size` section then failed (failure Other instead of ReadCrashed, no death, no respawn). I restored the check.

## Results (real counts)

**ASan** (`out/clang-asan`):

| Run | Result |
|---|---|
| `vtextension_helper_test "Hostility*"`, 4 runs, exit 0 | 6 cases / 216 assertions each |
| `vtextension_helper_test`, full | 151 cases (150 passed, 1 skipped), 2524 assertions |
| `vtextension_process_test` | 514 assertions, all passed |
| `vtextension_remote_test "RemoteBackend.*"` | 33 cases / 6479 assertions |
| `ctest --preset=clang-asan -R vtextension` | 5/5 passed |

- The bounds held on every run, with wide margins. Durations:

  | Rows / case | Time |
  |---|---|
  | crash, violation and cap rows | ≈ 20 ms |
  | stop-reading, stall-after-header (watchdog + grace) | ≈ 130 ms |
  | hang rows | ≈ 225 ms |
  | `hangingLoadOpensTheBreaker` | 0.6 s |

**TSan** (`out/clang-tsan`; rebuilt: `contour-extension-host`, the hostile helper, `vtextension_helper_test`, `vtextension_process_test`, `vtextension_remote_test`). Helpers are TSan-built too. Every run below reported 0 ThreadSanitizer warnings:

| Run | Result |
|---|---|
| `vtextension_helper_test "Hostility*"`, 3 runs | 216 assertions each |
| `vtextension_helper_test`, full | 151 cases (150 passed, 1 skipped) |
| `vtextension_process_test` | all passed |
| `vtextension_remote_test "RemoteBackend.release*"` | 4 cases |

**Windows** (clang-cl cross, `out/clangcl-debug`, -Werror; wine with own prefix `scratchpad/wine36`, mingw `zlib1.dll` on `WINEPATH`):
- Built clean: `vtextension_process_test`, `contour-extension-host-hostile`, `vtextension_helper_test`.

| Run | Result |
|---|---|
| `vtextension_helper_test.exe "Hostility*"`, 6 runs (3 before and 3 after the clang-tidy fixes), exit 0 | 6 cases / 216 assertions each |
| `vtextension_helper_test.exe`, full | 123 cases (121 passed, 2 skipped), 1723 assertions |
| `vtextension_process_test.exe` | 70 cases / 470 assertions |

- The flaky case: 0 of 30 after the fix, against 7 of 30 before.
- `wineserver -k` was run on my prefix only.

**clang-tidy**, by hand:

```
clang-tidy -p out/clang-asan --quiet src/contour-extension-host/testing/HostileHost.cpp src/vtextension/wasm/remote/Hostility_test.cpp src/vtextension/wasm/remote/process/testing/HelperComposition.cpp src/vtextension/wasm/remote/RemoteBackend_test.cpp src/vtextension/wasm/remote/process/ExtensionHostSupervisor_test.cpp
```

- First pass: 4 findings.
  - `bugprone-invalid-enum-default-initialization` ×2: `PduKind {}` as "no after". It is now `std::optional<PduKind>`.
  - `readability-avoid-return-with-void-value`.
  - `bugprone-random-generator-seed`. See Concerns.
- Re-run on `HostileHost.cpp`, which also covers `HostileModes.hpp`: 0 findings.

`/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` on every changed file: clean.

## Deviations

1. **The `HostileMode` enum was removed.** Rows are looked up by name, and each row carries its behaviour as data. An enum would have been a second place to edit per row.
2. **Without `--hostile`, the hostile binary serves.** Previously it refused. The brief's "launches after the first drop `--hostile`" requires this.
3. **The `RecordingLauncher` rewrite already existed** (from 3.1). Only `HelperComposition` gained the parameter.
4. **"Stop reading every parent channel"** stops each Contour→host pump before its next receive. A pump already blocked in a receive on another channel takes at most one more chunk and discards it. The suite uses one lane, so it is unaffected.

## Concerns

- **`bugprone-random-generator-seed`.** The brief requires `std::mt19937{0x5eed}`, a fixed seed, so that the garbage repeats.
  - The engine is now built in `garbageOf(seed)`, and `GarbageSeed` is passed in as a parameter. The check no longer fires.
  - This satisfies the check without a NOLINT, but by indirection: the seed is still a constant. A reviewer may prefer a fixed 64-byte table instead. That would also be deterministic, but it would drop the mt19937 the brief names.
- **The garbage row takes the protocol-violation path today.** The first mt19937 bytes decode as an immediate violation, at about 20 ms. If they ever decoded as `NeedMoreData`, the row would still end `HostCrashed` through the watchdog and the grace.
  - `oneViolationIsOneFault` has no watchdog: `Instance` runs on a `ManualClock`. In that case it would rely on the 60 s case deadline and fail rather than pass.
- **Fault counting.** The breaker cases do not assert `uncleanExits`. They assert fault counts and launches only.
- **macOS and FreeBSD.** `HostileHost.cpp` uses only portable calls plus a `_WIN32` branch. It is compiled only by CI on those platforms.

## Fix round 1

### Commits (on d9a624925)
- 01fd60f6d `docs(vtextension): say what the helper test compositions guard against` (item 6, other files)
- 4831c4bbf `test(vtextension): prove how each hostile host was stopped, and add four more` (items 1–5, and item 6 in Hostility_test.cpp)

### Items
1. **(Important) Kill-reason column.**
   - `HostileRow.end` is now a `HelperEnd`, which also names how the helper ended:
     - `Refused` → `kills[Requested] == 1`, every other reason 0;
     - `GraceExpired` → `kills[Grace] == 1`, others 0;
     - `StepOverran` → `kills[StepTimeout] == 1`, others 0;
     - `OnItsOwn` → Grace, StepTimeout, Dropped and Shutdown are all 0, and Requested is ≤ 1, plus one unclean exit.
   - `checkKills()` loops over every `KillReason` (it syncs through `killsFor`).
   - The row assignments:

     | Rows | HelperEnd |
     |---|---|
     | over-cap, bogus-size, garbage, wrong-version | Refused |
     | stop-reading, stall-after-header | GraceExpired |
     | the hang rows and stop-reading-at-release | StepOverran |
     | abort, segfault, exit-in-callhost, exit-before-hello | OnItsOwn |

   - Why OnItsOwn allows one Requested kill: when the parent sees the channel end, it calls `markDead()`, which posts a kill. That kill can arrive before the supervisor has reaped the crashed helper. It is counted, and does nothing, because the process has already exited. So it cannot be asserted to be 0 without a race.
   - **Mutation evidence:**
     - **M1.** The `sizeCap` check in `Codec.cpp` (readHeader, ~532) deleted. `Hostility.modes -c over-cap` fails: Requested is `0 == 1` and Grace is `1 == 0` (the watchdog and grace path).
     - **M2.** Parse-error detection broken: readHeader reports a bad or unknown kind as `NeedMoreData` until the whole frame has arrived. Two cases fail:
       - `-c garbage`: Requested `0 == 1`, Grace `1 == 0`;
       - `Hostility.garbageIsAViolationAtOnce`: `0 != 0`.
     - Both mutations were reverted, then rebuilt.
2. **(Minor) Fixed garbage table.**
   - `GarbageAnswer` (64 bytes) in `HostileModes.hpp`, commented as produced once from `std::mt19937{0x5eed}` (low byte of its first 64 outputs). It was generated with a scratch program.
   - The seed-as-parameter and `<random>` are gone from the helper.
   - New `Hostility.garbageIsAViolationAtOnce`: `frameSizeOf(GarbageAnswer, ToParent, Lane)` must be an error other than `NeedMoreData`.
3. **(Minor) Breaker cases check every helper ended.**
   - `HostileExtension` now borrows a `HelperComposition` owned by the case.
   - Each case ends with `checkEveryHelperEnded` (`awaitEveryHelperReaped()` plus `uncleanExits`).
   - Expected unclean exits:

     | Case | Unclean |
     |---|---|
     | oneCrash | 1 |
     | oneHang | 0 |
     | oneViolation | 0 |
     | repeatedCrashes | 3 |
     | hangingLoad | 0 |

4. **(Minor) New rows.** Two new `HostileAction`s: `Exit` and `ForwardWrongVersion` (decode the Hello, `version + 1`, re-encode and forward).

   | Row | Behaviour | Expected | HelperEnd |
   |---|---|---|---|
   | `exit-before-hello` | `_Exit(3)` at the Hello | load → HostUnavailable | OnItsOwn, unclean 1 |
   | `wrong-version` | real process, Hello version off by one | load → HostUnavailable | Refused |
   | `hang-at-create-runtime` | holds RuntimeCreated back | load → HostUnavailable after the handshake budget | StepOverran |
   | `stop-reading-at-release` | stops reading the control channel at the first Release after an Instantiated | see below | StepOverran |

   - **`stop-reading-at-release` (`Operation::ReleaseFlood`):**
     - The case creates 1000 instances, then drops all of them; every drop must take less than `DropBound` (100 ms).
     - The next `instantiate` fails `HostUnavailable` within [1100, 1400) ms. It times out in its blocking drain of the queued releases or in its own exchange, under the Instantiate watch.
     - That is one StepTimeout kill, then recovery.
     - It does not reach the 4096-release overflow kill: through a real channel that would need about 4400 objects. That path stays covered by `RemoteBackend.releaseOverflowKillsTheHelper` (scripted host).
5. **(Minor) Distinct budgets.**
   - Budgets: handshake 300 ms, load 700 ms, instantiate 1100 ms (grace 50 ms).
   - A `StepOverran` row names its `HostStep`. The case checks that `budget ≤ elapsed < budget + 300 ms`, so a hang ended by another step's budget falls outside the window.
   - Observed elapsed times: hang-at-handshake and hang-at-create-runtime ≈ 0.32 s, hang-at-load ≈ 0.73 s, hang-at-instantiate ≈ 1.13 s.
6. **(Minor) Plan IDs removed.**
   - In `Hostility_test.cpp` (`(D57)`, `(D53)`), the meaning is now stated in the comment.
   - The search of the branch's added lines under `src/` also found:
     - `ReviewFocusProcess_test.cpp` (D57);
     - `HelperComposition.cpp` (D39 ×2);
     - `HelperComposition.hpp` (D57);
     - `ProcessDefaultBackend.cpp` (D57 ×2).
   - All were rewritten. A re-run of the search over the changed files finds none.

### Results (real counts)
- **ASan:**

  | Run | Result |
  |---|---|
  | `vtextension_helper_test "Hostility*"`, 4 runs, exit 0 | 7 cases / 1370 assertions each |
  | `ctest --preset=clang-asan -R vtextension` | 5/5 |

- **TSan:**

  | Run | Result |
  |---|---|
  | `"Hostility*"`, 3 runs | 1370 assertions each, 0 warnings |
  | full `vtextension_helper_test` | 152 cases (151 passed, 1 skipped), 3678 assertions, 0 warnings |

- **Wine** (clang-cl cross, rebuilt): `"Hostility*"`, 3 runs, exit 0, 1370 assertions each.
- **clang-tidy** (`-p out/clang-asan --quiet`) on HostileHost.cpp, Hostility_test.cpp, HelperComposition.cpp, ProcessDefaultBackend.cpp and ReviewFocusProcess_test.cpp: 0 findings.
- **clang-format `--dry-run -Werror` and editorconfig-checker:** clean.

### Concerns
- `OnItsOwn` tolerates at most one `Requested` kill, because the parent's `markDead()` races the reap. It is a harmless no-op on an exited process, but the column cannot be "zero kills" for crash rows without that race.
- The budget windows' 300 ms slack includes the helper's start for load rows. A very slow machine could start the helper late enough to push hang-at-handshake past 600 ms. TSan and wine stayed well inside.
