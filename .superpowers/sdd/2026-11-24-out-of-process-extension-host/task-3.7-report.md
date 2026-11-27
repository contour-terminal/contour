# Task 3.7 report: Phase 3 gate

Status: DONE_WITH_CONCERNS. Base 4831c4bbf, head 6e99bc621. The whole-phase review (Step 7) is the
controller's. Step 5 (push and read the FreeBSD/macOS CI jobs) was not done, because the run rules forbid
pushing. Details are below and in the plan's "Phase-3 facts", which is filled in locally and not committed.

## Commits
1. c76fc82a1 `test(vtextension): time an overrunning host step from its own watch` (item 0).
   - Hostility_test.cpp wraps the connector (`StampingConnector`/`StampingHandle`), so each step's watch is
     stamped as it is armed.
   - A hang row's time now runs from its own step's stamp to the failure. Before, it ran from the start of the
     whole operation, which also counted the spawn, the handshake and the earlier steps.
   - Budgets and windows are unchanged: [300,600), [700,1000), [1100,1400). They do not overlap, so each row
     still proves which budget ended it.
   - The 300 ms slack now only has to cover the kill and its observation. `REQUIRE(armedAt)` also proves the
     step was really reached.
2. d8bedfc2a `test(vtextension): measure what a real extension host process costs`.
   - New `ProcessMeasure_test.cpp` in vtextension_helper_test, with four hidden `[.measure]` cases:
     `Process.measureRender`, `Process.measureRoundTrip`, `Process.measureSpawn` and `Process.measureFootprint`.
   - The round-trip workload moved into `wasm/testing/RenderMeasurement.hpp` (`measureRoundTrip`, `summarize`).
     `Remote.measureRoundTrip` now calls it, so both compositions run one workload.
3. 6e99bc621 `test(vtextension): let the hostile host crash without a core dump`. A bug the gate found, see
   below. The failing tests already existed: Hostility in the release and coverage trees.

## Step 1: ASan/LSan
- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: 7/7, run once at the end on the final head.

| Suite | Cases |
|---|---|
| vtextension_test | 397 |
| vtextension_wasmtime_test | 54 |
| vtextension_remote_test | 215 |
| vtextension_process_test | 77 (1 skipped) |
| vtextension_helper_test | 152 (1 skipped) |

- Both skips are the cap checks, which do not apply under sanitizers.
- `[wasm-contract]` has 41 cases, not the brief's 40. It runs on all four compositions: WAMR, Wasmtime, remote
  over an in-process host, and remote over the real helper.
- `[bundled]` has 14 cases on both remote compositions.
- No LSan or ASan report.
- D57 is live. I checked it by sabotage, which I reverted and never committed:
  - A use-after-free at the helper's exit, and separately 100 leaked allocations, each failed
    `Process.loadsAndCallsThroughARealHelper` at `uncleanExits == 0` (1 == 0).
  - The helper's report was printed on stderr.
  - A single leaked `new` in `main` was not reported. LSan's conservative scan still found the pointer, so this is
    not a gap in the gate.

## Step 2: TSan (out/clang-tsan, no suppressions)
| Run | Cases |
|---|---|
| `vtextension_test "[threading]"` | 22 |
| `vtextension_remote_test` | 215 |
| `vtextension_process_test` | 77 (1 skipped) |
| `vtextension_helper_test "~[.measure]"` | 152 (1 skipped) |

- Zero reports.
- The TSan tree was built before commit 3. That commit only changes the hostile host on plain builds, and
  sanitizer builds already disable core dumps.

## Bug found: crash rows in plain builds
- **Symptom.** In release and coverage builds, `Hostility.modes` `abort`/`segfault`,
  `Hostility.oneCrashIsOneFault` and `Hostility.repeatedCrashesOpenTheBreaker` failed every time:
  - `uncleanExits` was 0 where 1 was expected.
  - The supervisor counted one Grace kill.
- **Cause.** On this machine `core_pattern` pipes to systemd-coredump.
  - A crashed process stays alive while its core is written, which takes longer than the 50 ms interrupt grace.
  - So the supervisor kills it first and counts the kill as its own.
- **Confirmed.** With `ulimit -c 0`, all 7 hostility cases pass.
- **Why the sanitizer runs never saw it.** Sanitizer runtimes disable core dumps.
- **Fix (test side).** Before it crashes, the hostile host sets `RLIMIT_CORE` to 0 (POSIX).
- **Result.**
  - Release, full helper suite: 152/152.
  - Coverage: all five suites green.
  - ASan ctest: green.
  - Wine `[hostility]`: green.
- **Open product question (concern 1).** The real helper still dumps core when it crashes, so in a plain build:
  - A real crash can be counted as a Grace kill, or observed only once the dump is done.
  - The core holds guest memory.
  - The helper could set `RLIMIT_CORE` 0 and/or `PR_SET_DUMPABLE` 0 itself, as part of its confinement or the
    Phase 5 lockdown. I left this for a ruling.

## Step 3: Measurement (out/clang-release)
The helper runs in `TrapMode::Signals` (D33). Phase 2's in-process host ran `Software`. Each case was run three
times, with the phase-2 compositions run alongside for comparison.

**Plasma render, median of 1000 calls**

| Composition | Steady (µs) | Uncached (µs) |
|---|---|---|
| Real helper | 18.6 / 18.1 / 18.4 (≈ 18.4, p90 ~20–21) | 39.4 / 40.1 / 31.5 |
| In-process host (phase 2: 18.5–19.1 / 28.9–29.4) | 17.9 / 18.9 / 18.7 | 28.2 / 28.1 / 28.9 |
| In-process Wasmtime (phase 2: 2.8 / 12.4) | 2.8 / 2.8 / 3.2 | 12.8 / 12.9 / 12.8 |

- **Real helper, uncached.** Four more runs gave 32.6 / 25.4 / 26.9 / 25.3 µs. The median of all seven is
  31.5 µs, but the spread is wide. The machine is shared, with a load average around 2.3.
- **Real helper, load and start.** Load is 43 ms, against 58 ms for the in-process host; Signals mode compiles
  faster. Start is 0.3 ms.
- **What the boundary costs.** A steady render through a real process costs the same as through phase 2's
  in-process host. An uncached render costs 0–12 µs more, and varies a lot from run to run.

**Round trip** (10 000 `add` calls)
- Real helper: median 4.4 / 3.3 / 2.6 µs (≈ 3.3 µs); fastest 1.6–3.0 µs.
- In-process host, same runs: 2.7 / 2.6 / 3.2 µs. Phase 2 measured 3.2–3.4 µs.

**Spawn to `RuntimeCreated`** (launch, Hello and CreateRuntime; 20 incarnations)
- Median 1.29 / 1.23 / 1.18 ms, p90 ≤ 1.7 ms.

**Helper footprint**
Read from `/proc/<pid>/status`, with the caps applied. The numbers were identical in all three runs.

| State | VmData | VmRSS |
|---|---|---|
| Idle | 41.4 MiB (mostly the 4 × 10 MiB executor stacks) | 10.7–10.9 MiB |
| Plasma loaded | 45.8 MiB | 21.6 MiB |
| 16 plasma instances, each rendered once | 63.1 MiB | 22.6 MiB |

**D38 cap headroom**
- 1 GiB / 63.1 MiB = 16.2×, which clears the 4× bar. `DefaultDataBytes` stays at 1 GiB.
- Each instance adds about 1.1 MiB of VmData, so the cap would bind at roughly 900 instances in one helper.

## Step 4: Windows (clang-cl cross, out/clangcl-debug, -Werror; wine 11.20 with my own prefix)
- **Build.** These build clean:
  - `vtextension_process_test`, `extension_host_probe`, `contour-extension-host`,
    `contour-extension-host-hostile`, `vtextension_helper_test`.
  - `contour-extension-host` links Wasmtime's MSVC static library with the cross toolchain.
- **Run setup.** mingw's `zlib1.dll` is on `WINEPATH`, and the debug CRT DLLs are in bin, as in 3.1 and 3.4.

| Run | Result |
|---|---|
| `vtextension_process_test.exe`, 5 runs | 70 cases / 470 assertions each, all green |
| `vtextension_helper_test.exe "~[.measure]"`, 3 runs | 124 cases (122 passed, 2 skipped), 2882 assertions, all green |
| `[hostility]` after commit 3 | 7 cases / 1375 assertions, green |

- The process-test runs include the launcher's clean-environment, handle-list and kill-on-close-job cases. They
  also include the wait set, ControlPage, WakeSignal (with `signalNeverBlocks`), HelperArguments,
  `Channel.peerProcessExitIsClosed` and 29 Supervisor cases.
- The two helper-test skips are because this configuration builds no bundled or Rust guests.
- I measured under wine too, but it is a debug build, so the numbers are not comparable: round trip 65.8 µs,
  spawn 38.6 ms.
- I ran `wineserver -k` on my own prefix only.

## Step 6: Coverage (out/clang-coverage, five suites, llvm-cov gcov)
**Line coverage**

| File | Lines covered |
|---|---|
| ExtensionHostSupervisor.cpp | 95.9% of 458 |
| HostTimers.cpp | 100% of 47 |
| PosixLaunch.cpp | 86.0% of 136 |
| Process_linux.cpp | 86.9% of 84 |
| ProcessHostConnector.cpp | 95.6% of 68 |
| HelperLocation.cpp | 100% of 9 |
| LaneWatch.cpp | 100% of 62 |
| ControlPage_posix.cpp | 72.1% of 86 |
| WakeSignal_posix.cpp | 80.7% of 140 |
| HelperArguments.cpp | 95.8% of 95 |
| HostProcess.cpp | 94.8% of 97 |
| InterruptListener.cpp | 71.4% of 35 |
| HostBootstrap_posix.cpp | 92.0% of 25 |
| HelperIncarnation.cpp | 94.1% of 186 |
| RemoteBackend.cpp | 95.0% of 378 |
| main.cpp | 100% of 4 |
| **All together** | **91.9% of 1910** |

Functions: 333 of 348 (95.7%).

**Uncovered, and fine to leave**
- Syscall-failure branches and their errno mapping.
- Move operations of the RAII handle types (InterruptListener, ControlPage, WakeSender/Receiver).
- EINTR retries.
- The supervisor's degraded polling and its start-up failures.
- `exitStatusOfWait`, which only the FreeBSD/macOS reap path uses.

**Uncovered, and worth a test**
- The helper's `ProtocolViolation` and `HandlesUnusable` exits (HostProcess.cpp 103/109/120). No test sends the
  helper a malformed parent PDU.
- The interrupt listener's `WakeWait::PeerGone`.
- In HelperIncarnation:
  - The path where the connection is made, the helper is then refused, and its handle is terminated (86–88).
  - The outbox flush that finds the channel blocked and marks the incarnation dead (245).

## Not verified here
- **FreeBSD and macOS.** Their launchers, wait sets, control pages, wake signals and bootstraps compile only in
  CI. I did not push, per the run rules, so the CI results are still to come (Step 5).
- **Core dumps on CI runners.** The core-dump problem can appear on any runner that collects cores. The fix is
  POSIX-wide.

## Concerns
1. **Product core dumps.** The real helper dumps core when it crashes; this needs a ruling (see the bug section).
2. **Uncached render through a real process is noisy.** Medians ran from 25 to 40 µs. Measure it on a quiet
   machine before gate 9.2 (Task 6.5).
3. **Step 5 is outstanding.** It needs a push, which the run rules forbid me to do.
4. **Deviation: extra release targets.** I also built `vtextension_remote_test` and `vtextension_wasmtime_test`
   in the release tree. That let me re-run phase 2's compositions next to phase 3's in the same session, so the
   comparison is not against old numbers.
