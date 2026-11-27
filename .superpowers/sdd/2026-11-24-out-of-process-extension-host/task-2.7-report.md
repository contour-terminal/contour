# Task 2.7 — Phase 2 gate: report

Status: DONE_WITH_CONCERNS (gate green; Windows concerns below). Step 5 (phase review) left to the controller.

## Commits (on top of 4a0b5086d)
- 6b9e33c9e `test(vtextension): measure a render and a bare round trip through the host` — hidden
  `Remote.measureRender` and `Remote.measureRoundTrip` (`[.measure]`) at the end of
  `src/vtextension/wasm/remote/RemoteBackend_test.cpp`. clang-format, clang-tidy (manual run, clean),
  editorconfig-checker clean.
- d0cf0568b `fix(vtextension): leave NOMINMAX to the build on Windows` — `Channel_win32.cpp` and
  `Channel_test.cpp` redefined `NOMINMAX` over the top-level `-DNOMINMAX` (empty vs `1`): `-Wmacro-redefined`
  (clang-cl) / C4005 (MSVC) under -Werror, so the Windows channel did not compile at all. Found by the R12 step.

## Step 1 — ASan/LSan (out/clang-asan)
- `ctest --preset=clang-asan -R vtextension`: 3/3 pass (also re-run after each commit).
- Whole binaries: vtextension_test 384 cases / 13255 assertions; vtextension_wasmtime_test 52 / 1005;
  vtextension_remote_test 181 / 4130.
- `[wasm-contract]`: 40 cases (472 assertions) on each of the three.
- `[bundled]`: 14 cases (601 assertions) on both compositions (vtextension_test and vtextension_remote_test).
- No LSan report. The only LSan suppressions are the pre-existing project-wide GUI/driver ones
  (cmake/lsan-suppressions.txt; none touched by this branch).

## Step 2 — TSan (out/clang-tsan, reconfigured with PACE)
- `vtextension_test "[threading]"`: 21 cases, 1133 assertions, exit 0, 0 ThreadSanitizer warnings.
- `vtextension_remote_test "~[.measure]"`: 181 cases, 4130 assertions, exit 0, 0 warnings.
- No suppressions, no TSAN_OPTIONS.

## Step 3 — Whole-callback IPC overhead (out/clang-release, vtextension_remote_test only)
Plasma, 1000 timed renders after one warm-up, three runs; medians per call:

| | run 1 | run 2 | run 3 | Wasmtime in-process (re-run now) | Phase 1 |
|---|---|---|---|---|---|
| steady `render(120,1)` | 18.5 µs | 18.9 µs | 19.1 µs | 3.3 / 2.8 / 3.4 µs | 2.8 µs |
| uncached 120/119 | 29.0 µs | 29.4 µs | 28.9 µs | 12.5 / 12.6 / 13.1 µs | 12.4 µs |
| bare round trip (`add`, 10 000 calls) | 3.4 µs | 3.2 µs | 3.3 µs | — | — |

- p90: steady ~20 µs (24 µs once), uncached ~31 µs, round trip ~4 µs; fastest round trip 1.7 µs; 10 000 calls in
  ~33 ms. Load ~60 ms both ways (compilation); start 0.3 ms remote vs 0.1 ms.
- The boundary adds a near-constant ~16 µs per render (≈6.5x steady, ≈2.3x uncached) — about five round
  trips' worth (call, host calls, copy-path memory reads). That count is an inference from the numbers, not
  measured PDU by PDU.
- Caveat: the "host" is HostServer in this process over real channels (Wasmtime, software traps); a separate
  process adds cross-process scheduling on top. The Wasmtime in-process binary in out/clang-release was the
  one already built (01:26), not rebuilt; it reproduces Phase 1.

## Step 4 — Coverage (out/clang-coverage, the three suites, gcov line coverage via llvm-cov gcov)
| file | lines |
|---|---|
| wasm/remote/RemoteBackend.cpp | 94.0% of 299 |
| wasm/remote/RemoteKindState.cpp | 92.0% of 75 |
| wasm/remote/HelperIncarnation.cpp | 87.7% of 73 |
| wasm/remote/CopyPathMemory.cpp | 96.0% of 101 |
| wasm/remote/proto/Codec.cpp | 99.6% of 271 |
| wasm/remote/transport/Channel_posix.cpp | 90.0% of 60 |
| wasm/remote/transport/PduStream.cpp | 95.6% of 45 |
| wasm/host/HostServer.cpp | 95.1% of 385 |
| wasm/host/InProcessHost.cpp | 97.8% of 89 |
| wasm/host/ExecutorThread.cpp | 80.0% of 35 |
| ExtensionRegistry.cpp | 95.1% of 103 |
| **all of the above** | **94.9% of 1536** |

Unreached lines are error paths: ExecutorThread's move-assignment and thread-creation failure;
HelperIncarnation's connector failure and send failures other than OverCap.

## Windows step (R12) — cross build + wine
- `vtextension_test` does not cross-build with clang-cl, for reasons outside this work:
  1. Debug preset + WAMR: `MSVC_DEBUG_INFORMATION_FORMAT value 'ProgramDatabase' not known for this ASM compiler`
     (WAMR enables ASM; clang-cl as ASM). Worked around with `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`.
  2. WAMR derives its platform from `CMAKE_HOST_SYSTEM_NAME` → `linux` → `pthread.h` not found. Worked
     around with `-DWAMR_BUILD_PLATFORM=windows`. (Fixable in cmake/ContourThirdParties.cmake by passing the
     platform from `CMAKE_SYSTEM_NAME` — not done here.)
  3. WAMR `core/shared/platform/windows/win_file.c:1298`: a `// \??\ ` comment ending in backslash-space,
     which clang splices into the next line (MSVC does not) → `error: expected identifier`. Upstream WAMR;
     no flag avoids it. WAMR on Windows therefore builds with MSVC only, not clang-cl (also affects the native
     `clangcl-*` presets).
- Instead the three suites were built as a local, uncommitted target (test_main.cpp + Codec_test +
  Channel_test + PduStream_test linking `vtextension_remote_wire`, core::testing, Catch2), in
  `out/clangcl-debug` (`linux-to-windows-clangcl-debug`, `CONTOUR_EXTENSIONS=ON`, `CONTOUR_EXTENSIONS_BUNDLED=OFF`,
  `CONTOUR_FRONTEND_GUI=OFF`, deps symlinked from contour.agent-orchestrator/out/windows-cross-deps). The
  CMakeLists edit was reverted after the build.
- That build exposed the NOMINMAX break (fixed, d0cf0568b).
- wine 11.20 (Staging), own WINEPREFIX under the scratchpad, no DISPLAY: 22/22 cases, 7321 assertions, green in
  five full runs plus the filtered `"Channel.*,PduStream.*,PduCodec.*"` run;
  `Channel.shutdownEndsThePeersStream` and `Channel.endsAreNotInheritable` pass (`-s`, 8 assertions).
  `Channel.endsAreCloseOnExec` is `#ifndef _WIN32`. `wineserver -k` on the own prefix afterwards (it had
  already exited).
- Wine is not Windows: the pipe DACL, handle inheritance and shutdown semantics passing here is evidence,
  not proof. Real Windows CI (MSVC) must confirm.

## Phase 2 summary
**Performance.** A render through the remote backend costs ~16 µs more than in-process Wasmtime, steady
or uncached alike (18.9 vs 2.8 µs; 29.0 vs 12.4 µs); a bare round trip is 3.3 µs median. Measured with the
host in-process, so a real helper process will be somewhat slower. No threshold in this phase; gate 9.2
(orchestrator, 6.5) judges it. The overhead is per-round-trip, so the lever is fewer PDUs per render (Phase 4
replaces the copy path for Wasmtime, D9).

**Risk.**
- D9 — Load/CreateRuntime/Instantiate/Release serialize on the control channel (parent mutex); calls, host
  calls and copy-path memory traffic run on the calling thread's lane. Risk: control-path contention when many
  panes load at once, and copy-path memory reads dominate render latency (the ~16 µs above) until Phase 4.
  TSan clean over the whole remote suite.
- D10 — at most four lane users at once; a fifth concurrent lane thread fails its call with
  `CallFailure::Trap` and logs (pinned by `RemoteBackend.fifthLaneThreadFailsItsCall`). Risk: a pool wider
  than four lanes turns into extension traps rather than waiting; the worker pool must stay within it.
- D20 — breaker per registry entry with epoch-tagged admissions; a fault counts only in its current epoch.
  Risk: a bounded undercount in one race (two panes failing in the same epoch count once) — accepted by design;
  Runner's per-pane counting unchanged.
- Windows: the transport now compiles and passes under wine; the WAMR-on-clang-cl issues above mean the full
  extension suites have never run on a clang-cl Windows build, and the MSVC Windows CI job is the only real
  check.

**Coverage.** 94.9% of 1536 lines across wasm/remote/, wasm/host/ and ExtensionRegistry.cpp; lowest
ExecutorThread.cpp 80.0% (35 lines) and HelperIncarnation.cpp 87.7%, both error paths only.

## Concerns
1. WAMR does not build for Windows with clang-cl (host-derived platform; upstream win_file.c line splice). Not
   this branch's doing, but it blocks the R12 route via `vtextension_test`. Suggest a ticket: pass
   `WAMR_BUILD_PLATFORM` from `CMAKE_SYSTEM_NAME`, and patch/report the win_file.c comment upstream.
2. The NOMINMAX break means Windows CI for this branch would have failed; it had not run yet.
3. A standalone engine-free wire test target (`vtextension_remote_wire_test`) would let the transport suites
   build on any Windows toolchain; a suggestion, not done (out of scope).
4. Measurement used an in-process host; the real-process figure arrives with Phase 3.
